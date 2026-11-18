#include "vcs/SvnProvider.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <set>

namespace lila::vcs
{
namespace svnx
{
std::string key(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    if (s.starts_with("./"))
        s = s.substr(2);
    return s.empty() ? std::string(".") : s;
}

fs::path canonicalPath(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? fs::absolute(p).lexically_normal() : c;
}

/** Enough XML for svn's --xml output: elements, attributes, text and entities. Tolerates truncated input. */
struct Node
{
    std::string name;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<Node> children;

    const Node* child(std::string_view n) const
    {
        for (const Node& c : children)
            if (c.name == n)
                return &c;
        return nullptr;
    }
    std::string attr(const std::string& n) const
    {
        auto it = attrs.find(n);
        return it == attrs.end() ? std::string() : it->second;
    }
    std::string childText(std::string_view n) const
    {
        const Node* c = child(n);
        return c ? c->text : std::string();
    }
};

std::string unescape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        size_t semi = s[i] == '&' ? s.find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos)
        {
            out += s[i];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#')
        {
            unsigned long cp = ent[1] == 'x' ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                             : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
            if (cp < 0x80)
                out += char(cp);
            else if (cp < 0x800)
                out += {char(0xC0 | (cp >> 6)), char(0x80 | (cp & 0x3F))};
            else if (cp < 0x10000)
                out += {char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F))};
            else
                out += {char(0xF0 | (cp >> 18)), char(0x80 | ((cp >> 12) & 0x3F)), char(0x80 | ((cp >> 6) & 0x3F)),
                        char(0x80 | (cp & 0x3F))};
        }
        else
        {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

Node parseXml(std::string_view s)
{
    Node root;
    std::vector<Node*> stack{&root};
    size_t i = 0;
    while (i < s.size())
    {
        size_t lt = s.find('<', i);
        if (lt == std::string_view::npos)
            lt = s.size();
        if (lt > i)
            stack.back()->text += unescape(s.substr(i, lt - i));
        if (lt >= s.size())
            break;
        if (s.compare(lt, 4, "<!--") == 0)
        {
            size_t end = s.find("-->", lt);
            i = end == std::string_view::npos ? s.size() : end + 3;
            continue;
        }
        size_t gt = s.find('>', lt);
        if (gt == std::string_view::npos)
            break;
        std::string_view tag = s.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!')
            continue;
        if (tag[0] == '/')
        {
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        bool selfClose = tag.back() == '/';
        if (selfClose)
            tag.remove_suffix(1);
        Node n;
        size_t p = 0;
        while (p < tag.size() && !std::isspace((unsigned char)tag[p]))
            ++p;
        n.name = std::string(tag.substr(0, p));
        while (p < tag.size())
        {
            while (p < tag.size() && std::isspace((unsigned char)tag[p]))
                ++p;
            size_t eq = tag.find('=', p);
            if (eq == std::string_view::npos || eq + 1 >= tag.size())
                break;
            std::string attrName = trim(std::string(tag.substr(p, eq - p)));
            char quote = tag[eq + 1];
            size_t close = tag.find(quote, eq + 2);
            if (close == std::string_view::npos)
                break;
            n.attrs[attrName] = unescape(tag.substr(eq + 2, close - eq - 2));
            p = close + 1;
        }
        stack.back()->children.push_back(std::move(n));
        if (!selfClose)
            stack.push_back(&stack.back()->children.back());
    }
    return root;
}

/** svn's ISO 8601 UTC date ("2026-11-19T01:02:03.123456Z") as unix seconds. */
int64_t parseDate(const std::string& d)
{
    int y = 0, mo = 0, da = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(d.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &da, &h, &mi, &se) != 6)
        return 0;
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + da - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

std::string exeOf(const SvnSettings& s)
{
    if (!s.exe.empty())
        return s.exe;
    if (fs::path p = findExecutable("svn"); !p.empty())
        return p.string();
#ifdef _WIN32
    std::error_code ec;
    for (const char* p : {"C:/Program Files/TortoiseSVN/bin/svn.exe", "C:/Program Files/SlikSvn/bin/svn.exe"})
        if (fs::is_regular_file(p, ec))
            return p;
#endif
    return "svn";
}

/** A trailing @ stops svn reading an @ inside the name as a peg revision. */
std::string peg(const std::string& path)
{
    return path + "@";
}

bool changed(const std::string& item, const std::string& props)
{
    static const std::set<std::string> quiet{"", "normal", "unversioned", "ignored", "none", "external"};
    return !quiet.contains(item) || (props != "" && props != "none" && props != "normal");
}

/** A file of targets for --targets, removed when this goes out of scope. */
struct TargetsFile
{
    fs::path path;

    explicit TargetsFile(const std::vector<std::string>& targets)
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("lila_svn_targets_" + std::to_string(rd()) + ".txt");
        std::ofstream out(path, std::ios::binary);
        for (const std::string& t : targets)
            out << peg(t) << '\n';
    }
    ~TargetsFile()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
};
} // namespace svnx

SvnSettings SvnSettings::fromJson(const json& j)
{
    SvnSettings s;
    if (j.is_object() && j.contains("exe") && j["exe"].is_string())
        s.exe = j["exe"].get<std::string>();
    return s;
}

json SvnSettings::toJson() const
{
    return {{"exe", exe}};
}

SvnProvider::SvnProvider(fs::path root, SvnSettings settings, std::shared_ptr<ProcessRunner> runner)
    : mRoot(svnx::canonicalPath(root)), mSettings(std::move(settings)), mRunner(std::move(runner))
{
    mRootKey = svnx::key(mRoot.generic_string());
}

std::optional<fs::path> SvnProvider::findRoot(const fs::path& dir)
{
    LILA_ZONE();

    std::error_code ec;
    fs::path d = svnx::canonicalPath(dir);
    while (!d.empty())
    {
        if (fs::is_directory(d / ".svn", ec))
            return d;
        if (d == d.parent_path())
            break;
        d = d.parent_path();
    }
    return std::nullopt;
}

std::string SvnProvider::run(const std::vector<std::string>& args, int* exitCode, int timeoutMs)
{
    LILA_ZONE();

    ProcessSpec spec;
    spec.argv = {svnx::exeOf(mSettings), "--non-interactive"};
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.cwd = mRoot;
    // English messages, since commit and update report their revision only in text.
    spec.env = {{"LANGUAGE", "en"}, {"LC_MESSAGES", "C"}};
    spec.timeoutMs = timeoutMs;
    ProcessResult r = mRunner->run(spec);
    if (!r.started)
        throw VcsError("svn: the command-line client is not installed or not on PATH (TortoiseSVN: rerun its installer "
                       "and add the command line client tools) " + trim(r.err));
    if (r.timedOut)
        throw VcsError("svn " + (args.empty() ? std::string() : args.front()) + ": timed out");
    if (exitCode)
    {
        *exitCode = r.exitCode;
        return r.out;
    }
    if (r.exitCode != 0)
    {
        std::string msg = trim(r.err.empty() ? r.out : r.err);
        throw VcsError("svn " + (args.empty() ? std::string() : args.front()) + ": " +
                       (msg.empty() ? "exited with code " + std::to_string(r.exitCode) : msg));
    }
    return r.out;
}

std::string SvnProvider::rel(const fs::path& file) const
{
    std::string f = svnx::canonicalPath(file).generic_string();
    std::string k = svnx::key(f);
    if (k == mRootKey)
        return ".";
    if (k.size() > mRootKey.size() && k.compare(0, mRootKey.size(), mRootKey) == 0 && k[mRootKey.size()] == '/')
        return f.substr(mRootKey.size() + 1);
    if (mRootKey.ends_with('/') && k.starts_with(mRootKey))
        return f.substr(mRootKey.size());
    throw VcsError("svn: " + file.string() + " is outside the working copy at " + mRoot.string());
}

fs::path SvnProvider::abs(const std::string& r) const
{
    std::string g = r;
    std::replace(g.begin(), g.end(), '\\', '/');
    return (mRoot / fs::path(g)).lexically_normal();
}

std::vector<std::string> SvnProvider::targets(const std::vector<fs::path>& files) const
{
    std::vector<std::string> out;
    for (const fs::path& f : files)
        out.push_back(svnx::peg(rel(f)));
    return out;
}

json SvnProvider::info()
{
    LILA_ZONE();

    json j = {{"provider", name()}, {"root", mRoot.string()}, {"settings", mSettings.toJson()}};
    int code = 0;
    svnx::Node doc = svnx::parseXml(run({"info", "--xml", "."}, &code));
    const svnx::Node* infoNode = doc.child("info");
    const svnx::Node* entry = infoNode ? infoNode->child("entry") : nullptr;
    if (code == 0 && entry)
    {
        j["revision"] = entry->attr("revision");
        j["url"] = entry->childText("url");
        if (const svnx::Node* repo = entry->child("repository"))
            j["repositoryRoot"] = repo->childText("root");
    }
    return j;
}

bool SvnProvider::manages(const fs::path& dir)
{
    try
    {
        rel(dir);
        return true;
    }
    catch (const VcsError&)
    {
        return false;
    }
}

std::map<std::string, SvnProvider::Entry> SvnProvider::statusEntries(const std::vector<std::string>& rels, bool verbose,
                                                                     bool remote)
{
    LILA_ZONE();

    std::vector<std::string> base{"status", "--xml"};
    if (verbose)
        base.push_back("-v");
    std::vector<std::string> pegged;
    for (const std::string& r : rels)
        pegged.push_back(svnx::peg(r));

    auto parse = [](const std::string& xml, bool withRemote) {
        std::map<std::string, Entry> out;
        svnx::Node doc = svnx::parseXml(xml);
        const svnx::Node* st = doc.child("status");
        if (!st)
            return out;
        auto read = [&](const svnx::Node& group, const std::string& changelist) {
            for (const svnx::Node& e : group.children)
            {
                if (e.name != "entry")
                    continue;
                Entry en;
                en.path = e.attr("path");
                en.changelist = changelist;
                en.remote = withRemote;
                if (const svnx::Node* wc = e.child("wc-status"))
                {
                    en.item = wc->attr("item");
                    en.props = wc->attr("props");
                    en.revision = wc->attr("revision");
                    if (const svnx::Node* l = wc->child("lock"))
                        en.lockToken = l->childText("token");
                }
                if (const svnx::Node* rs = e.child("repos-status"))
                {
                    en.reposItem = rs->attr("item");
                    en.reposProps = rs->attr("props");
                    if (const svnx::Node* l = rs->child("lock"))
                    {
                        en.reposLockOwner = l->childText("owner");
                        en.reposLockToken = l->childText("token");
                    }
                }
                out[svnx::key(e.attr("path"))] = std::move(en);
            }
        };
        for (const svnx::Node& g : st->children)
        {
            if (g.name == "target")
                read(g, {});
            else if (g.name == "changelist")
                read(g, g.attr("name"));
        }
        return out;
    };

    std::vector<std::string> args = base;
    args.insert(args.end(), pegged.begin(), pegged.end());
    // Unversioned targets make svn warn and exit non-zero after printing the rest.
    int code = 0;
    std::map<std::string, Entry> local = parse(run(args, &code), false);
    if (!remote)
        return local;

    std::vector<std::string> versioned;
    for (const std::string& r : rels)
    {
        auto it = local.find(svnx::key(r));
        if (it != local.end() && it->second.item != "unversioned" && it->second.item != "ignored" &&
            it->second.item != "added" && it->second.item != "none")
            versioned.push_back(svnx::peg(r));
    }
    {
        std::scoped_lock lock(mMutex);
        if (versioned.empty() || std::chrono::steady_clock::now() < mOfflineUntil)
            return local;
    }
    args = base;
    args.push_back("-u");
    args.insert(args.end(), versioned.begin(), versioned.end());
    std::string out;
    try
    {
        out = run(args, &code, 15000);
    }
    catch (const VcsError& e)
    {
        code = -1;
        out.clear();
        log::warn("{}", e.what());
    }
    if (code != 0)
    {
        std::scoped_lock lock(mMutex);
        mOfflineUntil = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        log::warn("svn: can't reach the repository for {}; showing local status, so out-of-date files and other "
                  "people's locks are unknown for the next minute", mRoot.string());
        return local;
    }
    std::map<std::string, Entry> withRemote = parse(out, true);
    for (auto& [k, e] : withRemote)
        local[k] = std::move(e);
    return local;
}

std::map<std::string, bool> SvnProvider::needsLock(const std::vector<std::string>& rels)
{
    LILA_ZONE();

    std::map<std::string, bool> out;
    if (rels.empty())
        return out;
    std::vector<std::string> args{"propget", "svn:needs-lock", "--xml"};
    for (const std::string& r : rels)
        args.push_back(svnx::peg(r));
    // Targets without the property make svn warn and exit non-zero; the ones that have it are still listed.
    int code = 0;
    svnx::Node doc = svnx::parseXml(run(args, &code));
    if (const svnx::Node* props = doc.child("properties"))
        for (const svnx::Node& t : props->children)
            if (t.name == "target")
                out[svnx::key(t.attr("path"))] = true;
    return out;
}

std::vector<FileStatus> SvnProvider::status(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    std::vector<FileStatus> out;
    if (files.empty())
        return out;
    std::vector<std::string> paths;
    for (const fs::path& f : files)
        paths.push_back(rel(f));

    std::map<std::string, Entry> entries = statusEntries(paths, true, true);
    std::vector<std::string> tracked;
    for (const std::string& p : paths)
    {
        auto it = entries.find(svnx::key(p));
        if (it != entries.end() && it->second.item != "unversioned" && it->second.item != "ignored" && it->second.item != "none")
            tracked.push_back(p);
    }
    std::map<std::string, bool> locking = needsLock(tracked);

    for (size_t i = 0; i < files.size(); ++i)
    {
        FileStatus s;
        s.path = files[i].string();
        std::string k = svnx::key(paths[i]);
        auto it = entries.find(k);
        if (it == entries.end() || it->second.item == "unversioned" || it->second.item == "ignored" || it->second.item == "none")
        {
            s.state = FileState::NotTracked;
            out.push_back(std::move(s));
            continue;
        }
        const Entry& e = it->second;
        s.action = e.item;
        s.changelist = e.changelist;
        s.haveRev = e.revision;
        s.exclusive = locking.contains(k);
        s.outOfDate = e.remote && ((!e.reposItem.empty() && e.reposItem != "none") ||
                                   (!e.reposProps.empty() && e.reposProps != "none"));
        bool theirsInRepo = !e.reposLockToken.empty() && e.reposLockToken != e.lockToken;
        s.lockedByMe = !e.lockToken.empty() && (!e.remote || e.reposLockToken == e.lockToken);
        s.lockedByOther = theirsInRepo;
        if (theirsInRepo && !e.reposLockOwner.empty())
            s.otherUsers.push_back(e.reposLockOwner);

        if (e.item == "added")
            s.state = FileState::Added;
        else if (e.item == "deleted" || e.item == "missing")
            s.state = FileState::Deleted;
        else if (svnx::changed(e.item, e.props) || s.lockedByMe)
            s.state = FileState::CheckedOut;
        else if (s.lockedByOther)
            s.state = FileState::Locked;
        else if (s.outOfDate)
            s.state = FileState::OutOfDate;
        else
            s.state = FileState::Synced;
        s.checkedOutByMe = s.state == FileState::CheckedOut || s.state == FileState::Added || s.state == FileState::Deleted;
        out.push_back(std::move(s));
    }
    return out;
}

OpResult SvnProvider::each(const char* verb, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    OpResult res;
    for (const fs::path& f : files)
    {
        try
        {
            run({verb, svnx::peg(rel(f))});
            res.files.push_back(f.string());
        }
        catch (const VcsError& e)
        {
            res.warnings.push_back(f.string() + ": " + e.what());
        }
    }
    return res;
}

OpResult SvnProvider::checkout(const std::vector<fs::path>& files, const std::string& changelist)
{
    LILA_ZONE();

    std::vector<std::string> paths;
    for (const fs::path& f : files)
        paths.push_back(rel(f));
    std::map<std::string, bool> locking = needsLock(paths);
    std::vector<fs::path> toLock;
    OpResult res;
    for (size_t i = 0; i < files.size(); ++i)
    {
        std::error_code ec;
        fs::file_status st = fs::status(files[i], ec);
        const bool writable = !ec && (st.permissions() & fs::perms::owner_write) != fs::perms::none;
        // A writable svn:needs-lock file is one whose lock this working copy already holds.
        if (locking.contains(svnx::key(paths[i])) && !writable)
        {
            toLock.push_back(files[i]);
            continue;
        }
        // Without svn:needs-lock a read-only bit is only local, so editing just needs it cleared.
        fs::permissions(files[i], fs::perms::owner_write, fs::perm_options::add, ec);
        res.files.push_back(files[i].string());
    }
    OpResult locked = each("lock", toLock);
    res.files.insert(res.files.end(), locked.files.begin(), locked.files.end());
    res.warnings = std::move(locked.warnings);
    if (!changelist.empty() && changelist != "default" && !res.files.empty())
    {
        std::vector<fs::path> done(res.files.begin(), res.files.end());
        OpResult moved = moveToChangelist(done, changelist);
        res.warnings.insert(res.warnings.end(), moved.warnings.begin(), moved.warnings.end());
    }
    return res;
}

OpResult SvnProvider::add(const std::vector<fs::path>& files, const std::string& changelist)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"add", "--parents", "--force", "-q"};
    std::vector<std::string> t = targets(files);
    args.insert(args.end(), t.begin(), t.end());
    run(args);
    for (const fs::path& f : files)
        res.files.push_back(f.string());
    if (!changelist.empty() && changelist != "default")
    {
        OpResult moved = moveToChangelist(files, changelist);
        res.warnings = std::move(moved.warnings);
    }
    return res;
}

OpResult SvnProvider::revert(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    OpResult res;
    std::vector<fs::path> reverting, locked;
    for (const FileStatus& s : status(files))
    {
        if (s.state == FileState::NotTracked)
        {
            res.warnings.push_back(s.path + ": not tracked");
            continue;
        }
        reverting.push_back(s.path);
        if (s.lockedByMe)
            locked.push_back(s.path);
        res.files.push_back(s.path);
    }
    if (!reverting.empty())
    {
        std::vector<std::string> args{"revert", "--depth", "infinity", "-q"};
        std::vector<std::string> t = targets(reverting);
        args.insert(args.end(), t.begin(), t.end());
        run(args);
    }
    OpResult unlocked = each("unlock", locked);
    res.warnings.insert(res.warnings.end(), unlocked.warnings.begin(), unlocked.warnings.end());
    return res;
}

OpResult SvnProvider::remove(const std::vector<fs::path>& files, const std::string&)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"delete", "-q"};
    std::vector<std::string> t = targets(files);
    args.insert(args.end(), t.begin(), t.end());
    run(args);
    for (const fs::path& f : files)
        res.files.push_back(f.string());
    return res;
}

OpResult SvnProvider::move(const fs::path& from, const fs::path& to, const std::string&)
{
    LILA_ZONE();

    std::vector<std::string> t = targets({from, to});
    run({"move", "--parents", "-q", t[0], t[1]});
    OpResult res;
    res.files = {from.string(), to.string()};
    return res;
}

OpResult SvnProvider::submit(const std::string& changelist, const std::string& description, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    if (trim(description).empty())
        throw VcsError("svn: a commit needs a description");
    const bool named = !changelist.empty() && changelist != "default";
    std::vector<std::string> args{"commit", "-m", description};
    std::vector<std::string> commitTargets;
    OpResult res;

    if (named)
    {
        args.insert(args.end(), {"--changelist", changelist});
        if (files.empty())
            commitTargets.push_back(".");
        else
            for (const fs::path& f : files)
                commitTargets.push_back(rel(f));
        for (const Changelist& c : changelists({}))
            if (c.id == changelist)
                for (const std::string& f : c.files)
                    if (files.empty() || std::any_of(files.begin(), files.end(), [&](const fs::path& g) {
                            return svnx::key(rel(g)) == svnx::key(rel(f));
                        }))
                        res.files.push_back(f);
    }
    else if (!files.empty())
    {
        // A new file commits only with its added parent folders, which --depth empty would otherwise leave behind.
        std::set<std::string> seen;
        std::vector<std::string> ancestors;
        bool anyDir = false;
        std::error_code ec;
        for (const fs::path& f : files)
        {
            commitTargets.push_back(rel(f));
            seen.insert(svnx::key(commitTargets.back()));
            anyDir = anyDir || fs::is_directory(f, ec);
            res.files.push_back(f.string());
            for (fs::path p = fs::path(commitTargets.back()).parent_path(); !p.empty(); p = p.parent_path())
                if (seen.insert(svnx::key(p.generic_string())).second)
                    ancestors.push_back(p.generic_string());
        }
        if (!ancestors.empty())
        {
            std::vector<std::string> st{"status", "--xml", "--depth", "empty"};
            for (const std::string& a : ancestors)
                st.push_back(svnx::peg(a));
            int code = 0;
            svnx::Node doc = svnx::parseXml(run(st, &code));
            if (const svnx::Node* s = doc.child("status"))
                for (const svnx::Node& t : s->children)
                    for (const svnx::Node& e : t.children)
                        if (const svnx::Node* wc = e.child("wc-status"); e.name == "entry" && wc &&
                                                                         (wc->attr("item") == "added" || wc->attr("item") == "replaced"))
                            commitTargets.push_back(e.attr("path"));
        }
        if (!anyDir)
            args.insert(args.end(), {"--depth", "empty"});
    }
    else
    {
        std::vector<Changelist> lists = changelists({});
        const Changelist* def = lists.empty() ? nullptr : &lists.front();
        if (!def || def->files.empty())
            throw VcsError("svn: nothing to submit in the default changelist");
        res.files = def->files;
        if (lists.size() == 1)
        {
            commitTargets.push_back(".");
        }
        else
        {
            // Named changelists exist, so list the default's items and keep the named ones out.
            for (const std::string& f : def->files)
                commitTargets.push_back(rel(f));
            args.insert(args.end(), {"--depth", "empty"});
        }
    }

    svnx::TargetsFile list(commitTargets);
    args.insert(args.end(), {"--targets", list.path.string()});
    std::string out = run(args);
    std::smatch m;
    if (std::regex_search(out, m, std::regex(R"(Committed revision (\d+))")))
        res.change = m[1].str();
    else if (trim(out).empty())
        res.warnings.push_back("svn: nothing was committed");
    return res;
}

OpResult SvnProvider::sync(const std::vector<fs::path>& files, const fs::path&)
{
    LILA_ZONE();

    std::vector<std::string> args{"update", "--accept", "postpone"};
    if (files.empty())
        args.push_back(".");
    else
    {
        std::vector<std::string> t = targets(files);
        args.insert(args.end(), t.begin(), t.end());
    }
    std::string out = run(args);

    OpResult res;
    size_t at = 0;
    while (at < out.size())
    {
        size_t end = out.find('\n', at);
        std::string line = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? out.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_search(line, m, std::regex(R"((?:Updated to|At) revision (\d+))")))
        {
            res.change = m[1].str();
            continue;
        }
        // Update lines are four status columns, a space, then the path.
        if (line.size() <= 5 || line[4] != ' ' ||
            !std::all_of(line.begin(), line.begin() + 4, [](char c) { return std::strchr("ADUCGERB ", c) != nullptr; }))
            continue;
        std::string path = abs(trim(line.substr(5))).string();
        if (line.substr(0, 4).find('C') != std::string::npos)
            res.warnings.push_back(path + ": conflict, resolve it before committing");
        else
            res.files.push_back(path);
    }
    return res;
}

std::vector<Changelist> SvnProvider::changelists(const fs::path&)
{
    LILA_ZONE();

    Changelist def{"default", "Pending changes", "", false, {}};
    std::map<std::string, Changelist> named;
    std::map<std::string, Entry> entries = statusEntries({"."}, false, false);
    for (const auto& [k, e] : entries)
    {
        if (!svnx::changed(e.item, e.props) && e.lockToken.empty())
            continue;
        if (k == "." && !svnx::changed(e.item, e.props))
            continue;
        std::string file = abs(e.path).string();
        if (e.changelist.empty())
        {
            def.files.push_back(file);
            continue;
        }
        Changelist& c = named[e.changelist];
        c.id = e.changelist;
        c.description = e.changelist;
        c.files.push_back(file);
    }
    std::vector<Changelist> out{def};
    for (auto& [n, c] : named)
        out.push_back(std::move(c));
    return out;
}

std::vector<Revision> SvnProvider::history(const fs::path& file, int maxCount)
{
    LILA_ZONE();

    svnx::Node doc = svnx::parseXml(run({"log", "--xml", "-l", std::to_string(std::max(1, maxCount)), svnx::peg(rel(file))}));
    std::vector<Revision> out;
    const svnx::Node* log = doc.child("log");
    if (!log)
        return out;
    for (const svnx::Node& e : log->children)
    {
        if (e.name != "logentry")
            continue;
        Revision r;
        r.revision = e.attr("revision");
        r.change = r.revision;
        r.user = e.childText("author");
        r.time = svnx::parseDate(e.childText("date"));
        r.description = trim(e.childText("msg"));
        out.push_back(std::move(r));
    }
    return out;
}

void SvnProvider::fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest)
{
    LILA_ZONE();

    std::string blob = run({"cat", "-r", revision, svnx::peg(rel(file))});
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(blob.data(), std::streamsize(blob.size())))
        throw VcsError("svn: can't write " + dest.string());
}

OpResult SvnProvider::lock(const std::vector<fs::path>& files)
{
    return each("lock", files);
}

OpResult SvnProvider::unlock(const std::vector<fs::path>& files)
{
    return each("unlock", files);
}

std::string SvnProvider::createChangelist(const std::string& description, const fs::path&)
{
    std::string n = trim(description);
    if (n.empty())
        throw VcsError("svn: a changelist needs a name");
    if (n == "default")
        throw VcsError("svn: \"default\" is the changes outside any changelist; pick another name");
    return n;
}

OpResult SvnProvider::moveToChangelist(const std::vector<fs::path>& files, const std::string& changelist)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"changelist", "-q"};
    if (changelist.empty() || changelist == "default")
        args.push_back("--remove");
    else
        args.push_back(changelist);
    std::vector<std::string> t = targets(files);
    args.insert(args.end(), t.begin(), t.end());
    run(args);
    for (const fs::path& f : files)
        res.files.push_back(f.string());
    return res;
}
} // namespace lila::vcs
