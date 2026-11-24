// The style library: shipped JSON styles, the user's and the workspace's, read into one table any thread can look up.

#include "BuildingsScene.h"

#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Workspace.h>

#include <atomic>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace buildings
{
namespace
{
std::mutex& libMutex()
{
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<const Style>>& lib()
{
    static std::map<std::string, std::shared_ptr<const Style>> l;
    return l;
}
fs::path& shippedPath()
{
    static fs::path p;
    return p;
}
std::atomic<uint64_t> gRevision{1};

void readDir(const fs::path& dir, const std::string& source, std::map<std::string, std::shared_ptr<const Style>>& into)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        try
        {
            std::ifstream in(f);
            const json j = json::parse(in);
            std::string err;
            Style s = Style::fromJson(j, &err);
            if (!err.empty())
            {
                lila::log::warn("Buildings: {}: {}", f.generic_string(), err);
                continue;
            }
            s.source = source;
            const std::string id = s.id;
            into[id] = std::make_shared<const Style>(std::move(s));
        }
        catch (const std::exception& e)
        {
            lila::log::warn("Buildings: style {} unreadable: {}", f.generic_string(), e.what());
        }
    }
}
} // namespace

fs::path userStyleDir() { return lila::PrefStore::get().userDir() / "building-styles"; }

fs::path workspaceStyleDir()
{
    const lila::Workspace& ws = lila::Workspace::get();
    return ws.isOpen() ? ws.root() / "BuildingStyles" : fs::path();
}

void loadStyles(const fs::path& shippedDir)
{
    shippedPath() = shippedDir;
    reloadStyles();
}

void reloadStyles()
{
    std::map<std::string, std::shared_ptr<const Style>> next;
    readDir(shippedPath(), "shipped", next);
    readDir(userStyleDir(), "user", next);
    readDir(workspaceStyleDir(), "workspace", next);
    {
        std::lock_guard lock(libMutex());
        lib() = std::move(next);
    }
    ++gRevision;
    clearGenerationCache();
}

std::shared_ptr<const Style> findStyle(const std::string& id)
{
    std::lock_guard lock(libMutex());
    auto it = lib().find(id);
    return it == lib().end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const Style>> allStyles()
{
    std::vector<std::shared_ptr<const Style>> out;
    {
        std::lock_guard lock(libMutex());
        for (auto& [id, s] : lib())
            out.push_back(s);
    }
    // Shipped ones in menu order first, then the rest by id.
    const std::vector<std::string>& order = shippedStyleIds();
    std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        auto ia = std::find(order.begin(), order.end(), a->id), ib = std::find(order.begin(), order.end(), b->id);
        if (ia != ib)
            return ia < ib;
        return a->id < b->id;
    });
    return out;
}

uint64_t stylesRevision() { return gRevision.load(); }

fs::path saveStyle(const Style& s, const std::string& scope)
{
    const fs::path dir = scope == "workspace" ? workspaceStyleDir() : userStyleDir();
    if (dir.empty())
        throw std::runtime_error("no workspace is open; save the style to the user folder");
    fs::create_directories(dir);
    std::string file;
    for (char c : s.id)
        file += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    const fs::path f = dir / (file + ".json");
    std::ofstream out(f);
    out << s.toJson().dump(2);
    out.close();
    reloadStyles();
    return f;
}
} // namespace buildings
