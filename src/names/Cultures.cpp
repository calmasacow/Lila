// Naming cultures: street and district words (Settlement's cultures and mexican) and the vocabularies places and
// businesses are named from, mexican and english.

#include "names/Names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <stdexcept>

namespace lila::names
{
namespace
{
using Words = std::vector<const char*>;

/** Words a culture builds street and district names from: roots, and per class the words that go before or after them. */
struct Culture
{
    const char* name;
    Words roots;
    std::array<Words, 5> classWords; // arterial, collector, local, alley, path
    bool wordFirst = false;          // "Rue X" rather than "X Street"
    Words districtRoots;
    Words districtEnds; // joined onto a root ("Ash" + "ford"); empty: the root alone
    const char* districtPrefix = "";
    const char* vocabulary = "english";
};

const std::vector<Culture>& all()
{
    static const std::vector<Culture> c = {
        {"oldEnglish",
         {"Mill", "Church", "Market", "Bridge", "Castle", "Abbey", "Kings", "Queens", "Bishops", "Friars", "Tanners", "Weavers",
          "Smiths", "Coopers", "Bakers", "Cheap", "Wool", "Salt", "Fish", "Horse", "Sheep", "Barley", "Oak", "Ash", "Elm",
          "Thorn", "Holly", "Willow", "Hazel", "Rose", "Swan", "Crown", "Mint", "Cross", "Chapel", "Priory", "Manor", "Hall",
          "Green", "Moor", "Fen", "Marsh", "Brook", "Well", "Spring", "Stone", "Flint", "Chalk", "Hay", "Corn", "Malt", "Bell",
          "Lamb", "Fox", "Hart", "Raven", "Pudding", "Silver", "Copper", "Iron", "Water", "Wind", "High", "Broad", "Long",
          "Narrow", "Old", "Shambles", "Butchers", "Glovers", "Potters", "Ropers", "Dyers", "Mercers", "Chandlers"},
         {Words{"Street", "Road", "Way", "Gate"}, Words{"Street", "Lane", "Row", "Gate"}, Words{"Lane", "Row", "Close", "Yard", "Court"},
          Words{"Alley", "Passage", "Wynd", "Ginnel", "Snicket"}, Words{"Walk", "Path", "Steps", "Twitten"}},
         false,
         {"North", "South", "East", "West", "Ash", "Oak", "Mill", "Kings", "Church", "Market", "Stone", "Thorn", "Wool", "Salt",
          "Brook", "Elm", "Hay", "Fox", "Raven", "Bell", "Bishops", "Holly", "Swan", "Marsh"},
         {"gate", "field", "ford", "bury", "wick", "ton", "ham", "stead", "cross", "moor", "dale", "side", "wood", "end", "hythe"}},
        {"latin",
         {"Appia", "Aurelia", "Flaminia", "Iulia", "Claudia", "Sacra", "Nova", "Lata", "Recta", "Salaria", "Tiburtina",
          "Ostiensis", "Cornelia", "Valeria", "Aemilia", "Cassia", "Domitia", "Augusta", "Minervae", "Martis", "Iovis",
          "Veneris", "Fortunae", "Mercurii", "Saturni", "Dianae", "Apollinis", "Vestae", "Fori", "Portae", "Fontis", "Pontis",
          "Castrorum", "Fabrorum", "Pistorum", "Lanariorum", "Vinariorum", "Argentariorum", "Tuscus", "Iugarius", "Patricius"},
         {Words{"Via"}, Words{"Via", "Vicus"}, Words{"Vicus"}, Words{"Angiportus", "Semita"}, Words{"Semita", "Clivus", "Scalae"}},
         true,
         {"Palatium", "Aventinus", "Caelius", "Esquiliae", "Viminalis", "Quirinalis", "Subura", "Velabrum", "Carinae",
          "Campus Martius", "Forum", "Emporium", "Transtiberim", "Collis Hortulorum", "Insula", "Circus", "Vicus Longus",
          "Argiletum", "Ianiculum", "Pincius"},
         {}},
        {"french",
         {"de la Paix", "du Marché", "de l'Église", "des Tanneurs", "du Château", "de la Fontaine", "des Fleurs", "du Moulin",
          "Saint-Jacques", "Saint-Martin", "Saint-Michel", "Saint-Denis", "Saint-Honoré", "Saint-Antoine", "des Orfèvres",
          "de la Poste", "du Pont", "des Écoles", "Victor Hugo", "Voltaire", "Molière", "des Lilas", "des Peupliers",
          "du Faubourg", "de la Gare", "des Halles", "de la République", "de la Liberté", "Lafayette", "Colbert", "Pasteur",
          "des Arts", "du Temple", "des Jardins", "de la Croix", "du Puits", "des Remparts", "des Carmes", "des Bouchers",
          "de la Monnaie", "des Cordeliers"},
         {Words{"Boulevard", "Avenue"}, Words{"Rue", "Avenue"}, Words{"Rue"}, Words{"Ruelle", "Passage", "Impasse"},
          Words{"Allée", "Sentier", "Escalier"}},
         true,
         {"Saint-Germain", "Latin", "du Marais", "des Halles", "de la Gare", "du Port", "Saint-Paul", "des Batignolles",
          "de Montmartre", "Saint-Roch", "du Temple", "de la Cité", "Saint-Victor", "des Ternes", "de Grenelle", "de Belleville",
          "Saint-Lazare", "de la Roquette", "des Invalides", "de l'Arsenal"},
         {},
         "Quartier "},
        {"spanish",
         {"Mayor", "Real", "del Sol", "de la Luna", "del Carmen", "de San Juan", "de Santiago", "de la Paz", "del Rosario",
          "de los Reyes", "de las Flores", "del Mercado", "de la Iglesia", "del Río", "de la Fuente", "del Puente", "de Alcalá",
          "de Toledo", "de Cervantes", "de la Constitución", "de la Merced", "de San Francisco", "de los Olivos", "del Pilar",
          "de la Victoria", "del Castillo", "de la Plata", "de los Herreros", "de la Cruz", "del Puerto", "de las Huertas",
          "de los Tintes", "del Agua", "de la Feria"},
         {Words{"Avenida", "Paseo"}, Words{"Calle"}, Words{"Calle"}, Words{"Callejón", "Pasaje"}, Words{"Sendero", "Escalinata"}},
         true,
         {"de San Juan", "del Carmen", "de Santa Cruz", "Alto", "Bajo", "del Puerto", "de la Merced", "de Triana", "del Albaicín",
          "de la Catedral", "de San Pedro", "de la Judería", "de San Lorenzo", "del Castillo", "de las Huertas", "de San Marcos",
          "de la Magdalena", "del Pilar"},
         {},
         "Barrio ",
         "mexican"},
        {"usGrid",
         {"Main", "Oak", "Maple", "Elm", "Pine", "Cedar", "Washington", "Lincoln", "Jefferson", "Madison", "Franklin", "Park",
          "Lake", "Hill", "Church", "Center", "Market", "Union", "Liberty", "Spring", "Walnut", "Cherry", "Chestnut", "Broadway",
          "Highland", "Ridge", "River", "Mill", "Jackson", "Adams", "Grant", "Monroe", "Locust", "Sycamore", "Hickory"},
         {Words{"Avenue", "Boulevard"}, Words{"Street", "Avenue"}, Words{"Street", "Drive", "Place"}, Words{"Alley"}, Words{"Walk", "Trail"}},
         false,
         {"Midtown", "Uptown", "Old Town", "Riverside", "Eastside", "Westside", "Northside", "Southside", "Hillcrest",
          "Lakeview", "Fairview", "Oak Park", "Mill District", "Arts District", "Warehouse District", "University Heights",
          "Brookside", "Greenwood", "Elmwood", "Ironbound"},
         {}},
        {"sector", {}, {Words{"Trunk"}, Words{"Corridor"}, Words{"Spur"}, Words{"Duct"}, Words{"Walkway"}}, true, {}, {}},
        {"mexican",
         {"Las Palmas", "Los Laureles", "Los Mangos", "Las Gaviotas", "del Pescador", "de los Almendros", "Las Jacarandas",
          "del Mar", "de la Ribera", "Los Tabachines", "Las Bugambilias", "del Carrizal", "de las Garzas", "Los Guayabos",
          "del Faro", "Los Pinos", "de la Loma", "del Ancla", "Las Arboledas", "de los Pescadores", "del Muelle", "La Marea",
          "de las Conchas", "Los Cedros", "del Salitre", "de la Ceiba", "Los Sauces", "Las Huertas", "del Arroyo", "Los Naranjos",
          "de la Brisa", "del Caracol", "Los Olivos", "de los Alfareros", "de los Herreros", "del Tule", "Las Amapas",
          "del Venado", "Los Guamúchiles", "de la Higuera", "del Coral", "Las Lomas", "de los Arrieros", "del Molino",
          "Las Golondrinas", "de las Dunas", "El Palmar", "de la Salina", "del Mezquite", "Los Ébanos", "de la Cantera"},
         {Words{"Avenida", "Bulevar", "Calzada"}, Words{"Avenida", "Calle"}, Words{"Calle", "Calle", "Privada"},
          Words{"Callejón", "Andador"}, Words{"Andador", "Sendero", "Escalinata"}},
         true,
         {"Las Palmas", "El Mirador", "Los Laureles", "Lomas del Mar", "Vista Alegre", "El Palmar", "Las Gaviotas", "Los Mangos",
          "La Ribera", "Las Arboledas", "El Encanto", "Los Tabachines", "Loma Linda", "Las Brisas", "El Salitre", "Los Almendros",
          "Las Garzas", "Villa del Sol", "El Carrizal", "Arboledas del Río", "Los Sauces", "La Ceiba", "Campo Alegre",
          "Las Jacarandas", "Los Cedros", "El Faro", "Bugambilias", "Puerta del Mar", "Lomas de la Higuera", "El Mezquital"},
         {},
         "Colonia ",
         "mexican"},
    };
    return c;
}

const Culture& cultureNamed(std::string_view name)
{
    for (const Culture& c : all())
        if (name == c.name)
            return c;
    std::string known;
    for (const Culture& c : all())
        known += (known.empty() ? "" : ", ") + std::string(c.name);
    throw std::runtime_error("unknown naming culture '" + std::string(name) + "' (" + known + ")");
}

const char* pick(const Words& w, uint64_t h) { return w[size_t(h % w.size())]; }

// Place and business vocabularies ------------------------------------------------------------------------------------

/** Pools a pattern draws from: {g} given name or nickname, {s} surname, {e} evocative, {n} nature, {a} saint, {p} a
 * person (given and surname), {#} a number. */
struct Vocab
{
    Words given, surnames, evocative, nature, saints, persons;
    std::map<std::string, Words, std::less<>> programs; // business program -> patterns
    std::map<Kind, Words> kinds;                         // kind -> patterns
    Words business;                                      // any business without a program of its own
};

const Vocab& mexicanVocab()
{
    static const Vocab v = [] {
        Vocab x;
        x.given = {"Lupita", "Chuy", "Toño", "Paco", "Memo", "Lalo", "Nacho", "Beto", "Pepe", "Chema", "Rosy", "Lety", "Chabela",
                   "Juanita", "Tere", "Nena", "Güero", "Flaco", "Chato", "Pancho", "Rigo", "Polo", "Neto", "Tavo", "Cuco",
                   "Lencho", "Rafa", "Moni", "Licha", "Cheli", "Kiko", "Fito", "Meche", "Goyo", "Chayo", "Yoli", "Tito", "Lolis"};
        x.surnames = {"Ibarra", "Valenzuela", "Osuna", "Zazueta", "Salazar", "Robles", "Navarro", "Medina", "Ochoa", "Rentería",
                      "Camacho", "Sandoval", "Urías", "Lizárraga", "Tirado", "Peraza", "Bojórquez", "Gastélum", "Rosales",
                      "Castro", "Lara", "Montes", "Ríos", "Vega", "Soto", "Cázarez", "Millán", "Rubio", "Avendaño", "Domínguez",
                      "Andrade", "Ponce", "Villaseñor", "Quiñónez", "Velarde", "Barraza", "Crespo", "Olivas", "Sepúlveda",
                      "Arámburo", "Loaiza", "Echeverría", "Montoya", "Cota", "Sauceda", "Verdugo", "Amarillas", "Mendívil"};
        x.evocative = {"La Esperanza", "El Paraíso", "La Estrella", "El Faro", "La Gaviota", "El Ancla", "La Palapa", "El Tiburón",
                       "La Sirena", "El Coyote", "La Herradura", "El Gallo de Oro", "La Bonita", "El Rinconcito", "La Güera",
                       "El Compadre", "La Ola", "El Pelícano", "La Marea", "El Caracol", "La Lupita", "El Barco", "La Brisa",
                       "El Delfín", "La Palmera", "El Venado", "La Golondrina", "El Mezquite", "La Escondida", "El Atardecer",
                       "La Fortuna", "El Sombrero", "La Concha", "El Marlín", "La Tortuga", "El Navegante", "La Amistad",
                       "El Arrecife", "La Ballena", "El Nopal", "La Luna Llena", "El Huachinango", "La Calandria", "El Jaguar"};
        x.nature = {"Las Palmas", "Los Pinos", "El Mirador", "Las Garzas", "Los Mangos", "Las Brisas", "El Encanto", "La Loma",
                    "Los Álamos", "Las Huertas", "El Pedregal", "La Ceiba", "El Guamúchil", "Los Sauces", "Las Dunas", "El Palmar",
                    "Los Tabachines", "Las Amapas", "El Salitre", "La Cantera", "Los Mezquites", "El Carrizal", "Las Conchas",
                    "La Higuera", "El Tule", "Los Guayabos", "Las Lomas", "El Cerrito", "La Ribera", "Los Cedros"};
        x.saints = {"San Rafael", "Santa Rosa", "San Judas", "Santa Lucía", "San Isidro", "San Martín", "Santa Teresa", "San Benito",
                    "San Lázaro", "Santa Clara", "San Ramón", "Santa Mónica", "San Pascual", "San Cayetano", "Santa Inés",
                    "San Sebastián", "Santa Bárbara", "San Gabriel", "Santa Elena", "San Bartolo"};
        x.persons = {"Ernesto Lizárraga", "Carmen Valenzuela", "Rodolfo Osuna", "Amalia Zazueta", "Ignacio Peraza", "Rosario Urías",
                     "Abelardo Tirado", "Leonor Gastélum", "Alfonso Rentería", "Herminia Bojórquez", "Eusebio Camacho",
                     "Margarita Robles", "Octavio Medina", "Delfina Ochoa", "Fausto Sandoval", "Elvira Rosales",
                     "Ramiro Echeverría", "Consuelo Arámburo", "Teodoro Loaiza", "Graciela Velarde"};
        auto& p = x.programs;
        p["taqueria"] = {"Taquería {g}", "Tacos {e}", "Taquería {e}", "Tacos {g}"};
        p["restaurant"] = {"Restaurante {e}", "Fonda {g}", "Cocina {g}", "Lonchería {g}", "Comedor {e}"};
        p["seafood"] = {"Mariscos {e}", "Mariscos {g}", "Ostionería {e}", "Cevichería {e}"};
        p["cafe"] = {"Café {e}", "Cafetería {s}", "Café {g}"};
        p["bar"] = {"Bar {e}", "Cantina {e}", "Bar {g}"};
        p["cantina"] = {"Cantina {e}", "Cantina {g}"};
        p["nightclub"] = {"Club {e}", "Disco {e}"};
        p["fastFood"] = {"Antojitos {g}", "Hot Dogs {g}", "Tortas {g}", "Hamburguesas {e}", "Cenaduría {g}"};
        p["bakery"] = {"Panadería {e}", "Panadería {s}", "Pastelería {g}"};
        p["iceCream"] = {"Nevería {e}", "Paletería {e}", "Raspados {g}"};
        p["tortilleria"] = {"Tortillería {e}", "Tortillería {g}"};
        p["pharmacy"] = {"Farmacia {a}", "Farmacia {s}", "Farmacia {e}", "Botica {a}"};
        p["grocery"] = {"Abarrotes {g}", "Abarrotes {e}", "Miscelánea {g}", "Tienda {e}", "Abarrotes {s}"};
        p["supermarket"] = {"Súper {s}", "Supermercado {e}", "Mercadito {e}"};
        p["butcher"] = {"Carnicería {s}", "Carnes {g}", "Carnicería {e}"};
        p["carParts"] = {"Refaccionaria {s}", "Refacciones {e}", "Autopartes {s}"};
        p["tyres"] = {"Llantera {g}", "Vulcanizadora {e}", "Llantas {s}"};
        p["carRepair"] = {"Taller Mecánico {s}", "Taller {g}", "Hojalatería y Pintura {s}", "Mofles {g}"};
        p["carWash"] = {"Autolavado {e}", "Lavado de Autos {g}"};
        p["hardware"] = {"Ferretería {s}", "Materiales {s}", "Ferretería {e}"};
        p["clothes"] = {"Boutique {g}", "Ropa {e}", "Novedades {g}"};
        p["shoes"] = {"Zapatería {s}", "Calzado {e}"};
        p["electronics"] = {"Celulares {e}", "Electrónica {s}", "Reparación de Celulares {g}"};
        p["furniture"] = {"Muebles {s}", "Mueblería {e}"};
        p["stationery"] = {"Papelería {e}", "Papelería {g}"};
        p["hairdresser"] = {"Estética {g}", "Barbería {g}", "Estética {e}"};
        p["laundry"] = {"Lavandería {e}", "Lavandería {g}"};
        p["bank"] = {"Banco {s}", "Financiera {e}", "Caja Popular {a}"};
        p["fuel"] = {"Gasolinera {e}", "Servicio {s}", "Gasolinería {e}"};
        p["hotel"] = {"Hotel {e}", "Hotel {s}", "Posada {e}", "Hotel Playa {e}"};
        p["motel"] = {"Motel {e}", "Auto Hotel {e}"};
        p["mall"] = {"Plaza {e}", "Centro Comercial {n}"};
        p["market"] = {"Mercado {e}", "Mercado Municipal {s}", "Mercado {n}"};
        p["liquor"] = {"Depósito {e}", "Vinos y Licores {s}", "Expendio {g}"};
        p["florist"] = {"Florería {g}", "Florería {e}"};
        p["jewelry"] = {"Joyería {s}"};
        p["optician"] = {"Óptica {s}", "Óptica {e}"};
        p["clinic"] = {"Consultorio Médico {s}", "Clínica {a}", "Clínica {s}"};
        p["dentist"] = {"Consultorio Dental {s}", "Dentista {s}"};
        p["vet"] = {"Veterinaria {e}", "Clínica Veterinaria {s}"};
        p["gym"] = {"Gimnasio {e}", "Gym {g}"};
        p["cinema"] = {"Cine {e}", "Cinema {e}"};
        p["church"] = {"Parroquia de {a}", "Templo de {a}", "Capilla de {a}"};
        p["cathedral"] = {"Catedral de {a}"};
        p["school"] = {"Escuela Primaria {p}", "Secundaria Técnica No. {#}", "Jardín de Niños {e}", "Colegio {a}"};
        p["university"] = {"Universidad {s}", "Instituto Tecnológico {e}", "Instituto {p}"};
        p["hospital"] = {"Hospital {a}", "Hospital General {e}", "Clínica {a}"};
        p["police"] = {"Comandancia de Policía, Sector {#}", "Policía Municipal, Base {#}"};
        p["fireStation"] = {"Estación de Bomberos No. {#}"};
        p["townhall"] = {"Palacio Municipal {e}", "Ayuntamiento {e}"};
        p["government"] = {"Oficina de Gobierno No. {#}", "Junta Municipal {e}", "Recaudación de Rentas No. {#}"};
        p["courthouse"] = {"Juzgado {#}", "Tribunal de {a}"};
        p["military"] = {"Zona Militar {#}", "Cuartel {e}"};
        p["postOffice"] = {"Oficina Postal No. {#}"};
        p["library"] = {"Biblioteca {p}"};
        p["museum"] = {"Museo {e}", "Museo {p}"};
        p["theatre"] = {"Teatro {p}", "Teatro {e}"};
        p["stadium"] = {"Estadio {e}", "Estadio {s}"};
        p["sports"] = {"Unidad Deportiva {s}", "Campo {e}"};
        p["port"] = {"Puerto {e}", "Terminal Marítima {e}"};
        p["ferryTerminal"] = {"Terminal de Transbordadores {e}"};
        p["marina"] = {"Marina {e}"};
        p["airport"] = {"Aeropuerto Internacional {p}", "Aeródromo {e}"};
        p["airstrip"] = {"Pista {e}", "Aeródromo {n}"};
        p["busStation"] = {"Central de Autobuses {e}", "Terminal {e}"};
        p["cemetery"] = {"Panteón {e}", "Panteón Municipal No. {#}", "Cementerio de {a}"};
        p["park"] = {"Parque {e}", "Jardín {a}", "Parque {n}"};
        p["office"] = {"Despacho {s}", "Oficinas {s}"};
        p["warehouse"] = {"Bodega {s}", "Almacenes {e}"};
        p["factory"] = {"Industrias {s}", "Procesadora {e}"};
        p["packingPlant"] = {"Empacadora {e}", "Empacadora {s}"};
        p["coldStorage"] = {"Frigorífico {e}"};
        p["truckYard"] = {"Transportes {s}", "Fletes {g}"};
        p["farm"] = {"Rancho {e}", "Campo Agrícola {e}", "Granja {e}"};
        p["greenhouse"] = {"Invernaderos {e}", "Agrícola {s}"};
        p["lighthouse"] = {"Faro {e}"};
        p["shop"] = {"Tienda {e}", "Comercial {s}", "Novedades {e}"};
        p["building"] = {"Edificio {e}", "Torre {e}", "Condominios {e}"};
        p["apartments"] = {"Condominios {e}", "Departamentos {n}", "Torre {e}"};
        p["mansion"] = {"Quinta {e}", "Villa {e}"};
        p["shipping"] = {"Naviera {e}", "Línea Marítima {s}", "Transportes Marítimos {e}", "Contenedores {s}"};
        p["cruiseLine"] = {"Cruceros {e}", "Cruceros {s}"};
        p["cruiseTerminal"] = {"Terminal de Cruceros {e}", "Muelle de Cruceros {s}"};
        p["customs"] = {"Aduana Marítima {e}", "Aduana {s}"};
        p["mausoleum"] = {"Familia {s} {s}", "Capilla Familiar {s} {s}", "Familia {s}"};
        x.business = {"Comercial {s}", "Tienda {e}", "Negocio {g}"};
        auto& k = x.kinds;
        k[Kind::Town] = {"{a} de {n}", "Villa {s}", "{n}", "Puerto {e}", "Real de {n}"};
        k[Kind::River] = {"Río {n}", "Río {s}", "Río {e}"};
        k[Kind::Stream] = {"Arroyo {e}", "Arroyo {n}"};
        k[Kind::Lake] = {"Laguna {e}", "Laguna {n}"};
        k[Kind::Reservoir] = {"Presa {p}", "Presa {e}"};
        k[Kind::Dam] = {"Cortina {p}", "Presa {p}"};
        k[Kind::Island] = {"Isla {e}", "Isla de {n}"};
        k[Kind::Beach] = {"Playa {e}", "Playa {n}"};
        k[Kind::Bay] = {"Bahía {e}", "Ensenada {e}"};
        k[Kind::Mountain] = {"Cerro {e}", "Cerro {n}"};
        k[Kind::Park] = {"Parque {e}", "Jardín {a}"};
        k[Kind::Landmark] = {"Monumento {e}", "Glorieta {e}", "Mirador {e}"};
        return x;
    }();
    return v;
}

const Vocab& englishVocab()
{
    static const Vocab v = [] {
        Vocab x;
        x.given = {"Joe", "Rosie", "Sal", "Frank", "Dot", "Eddie", "Mo", "Lou", "Betty", "Gus", "Hank", "Ruby", "Stan", "Vera",
                   "Al", "Maggie", "Ned", "Pearl", "Rudy", "Tess", "Walt", "Ivy", "Mick", "Nell", "Otis", "Fern", "Clyde", "June"};
        x.surnames = {"Harlow", "Pemberton", "Whitlock", "Calloway", "Ashby", "Brennan", "Dunmore", "Fairley", "Garrick", "Holloway",
                      "Kessler", "Lindqvist", "Marlowe", "Norcross", "Oakes", "Prescott", "Quimby", "Radley", "Sutter", "Thorne",
                      "Underhill", "Vance", "Winslow", "Yardley", "Abernathy", "Blackwood", "Crowley", "Delancey", "Ellery", "Fenwick"};
        x.evocative = {"Bluebird", "Lucky Star", "Golden Anchor", "Silver Gull", "Red Lantern", "Sunset", "Harbor Light", "Driftwood",
                       "Black Pelican", "Copper Kettle", "Blue Marlin", "Morning Glory", "Old Mill", "Crescent", "Northstar",
                       "Seaside", "Lighthouse", "Sandpiper", "Iron Horse", "Wild Rose", "Tidewater", "Kingfisher", "High Noon",
                       "Lone Pine", "Stardust", "Firefly", "Riverbend", "Cobblestone", "Bayview", "Westwind"};
        x.nature = {"Pine Hollow", "Cedar Ridge", "Willow Creek", "Oak Bluff", "Maple Glen", "Birch Point", "Elm Grove", "Ash Vale",
                    "Fern Hill", "Heron Marsh", "Fox Run", "Stony Brook", "Sandy Hook", "Clear Water", "Eagle Rock", "Deer Lick"};
        x.saints = {"St. Anselm", "St. Brendan", "St. Cuthbert", "St. Dunstan", "St. Edith", "St. Felix", "St. Hilda", "St. Jude",
                    "St. Kevin", "St. Lucy", "St. Martin", "St. Oswald", "St. Agnes", "St. Ambrose"};
        x.persons = {"Edwin Harlow", "Martha Pemberton", "Arthur Whitlock", "Clara Calloway", "Henry Ashby", "Agnes Brennan",
                     "Walter Dunmore", "Edith Fairley", "Silas Garrick", "Ruth Holloway", "Oscar Kessler", "Mabel Lindqvist"};
        auto& p = x.programs;
        p["taqueria"] = {"{g}'s Tacos", "{e} Taco Stand"};
        p["restaurant"] = {"{g}'s Diner", "The {e} Grill", "{e} Kitchen", "{s}'s Restaurant"};
        p["seafood"] = {"{e} Seafood", "{g}'s Fish House", "The {e} Oyster Bar"};
        p["cafe"] = {"{e} Café", "{g}'s Coffee", "The {e} Coffee House"};
        p["bar"] = {"The {e}", "{g}'s Bar", "The {e} Tavern"};
        p["cantina"] = {"The {e} Saloon", "{g}'s Tavern"};
        p["nightclub"] = {"Club {e}", "The {e} Lounge"};
        p["fastFood"] = {"{g}'s Burgers", "{e} Drive-In", "{g}'s Hot Dogs"};
        p["bakery"] = {"{s} Bakery", "{e} Bakery", "{g}'s Bake Shop"};
        p["iceCream"] = {"{e} Creamery", "{g}'s Ice Cream"};
        p["tortilleria"] = {"{e} Tortilla Factory"};
        p["pharmacy"] = {"{s} Pharmacy", "{e} Drugs", "{s} Apothecary"};
        p["grocery"] = {"{g}'s Corner Store", "{e} Market", "{s} Grocery"};
        p["supermarket"] = {"{s} Foods", "{e} Supermarket"};
        p["butcher"] = {"{s} Butchers", "{g}'s Meats"};
        p["carParts"] = {"{s} Auto Parts", "{e} Parts Supply"};
        p["tyres"] = {"{g}'s Tire Shop", "{s} Tire & Wheel"};
        p["carRepair"] = {"{s} Motors", "{g}'s Garage", "{e} Auto Repair"};
        p["carWash"] = {"{e} Car Wash"};
        p["hardware"] = {"{s} Hardware", "{e} Lumber & Supply"};
        p["clothes"] = {"{e} Outfitters", "{g}'s Boutique"};
        p["shoes"] = {"{s} Shoes"};
        p["electronics"] = {"{e} Electronics", "{s} Radio & TV"};
        p["furniture"] = {"{s} Furniture", "{e} Home"};
        p["stationery"] = {"{e} Stationers"};
        p["hairdresser"] = {"{g}'s Barbershop", "{e} Salon"};
        p["laundry"] = {"{e} Laundromat"};
        p["bank"] = {"{s} Savings & Loan", "{e} Trust", "{s} Bank"};
        p["fuel"] = {"{e} Gas", "{s} Service Station"};
        p["hotel"] = {"Hotel {e}", "The {s} Hotel", "{e} Inn"};
        p["motel"] = {"{e} Motel", "{e} Motor Lodge"};
        p["mall"] = {"{e} Mall", "{e} Plaza"};
        p["market"] = {"{e} Market", "{s} Market Hall"};
        p["liquor"] = {"{e} Liquors", "{g}'s Bottle Shop"};
        p["florist"] = {"{g}'s Flowers"};
        p["jewelry"] = {"{s} Jewelers"};
        p["optician"] = {"{s} Optical"};
        p["clinic"] = {"{s} Medical Center", "{a} Clinic"};
        p["dentist"] = {"{s} Dental"};
        p["vet"] = {"{e} Animal Hospital"};
        p["gym"] = {"{e} Fitness", "{g}'s Gym"};
        p["cinema"] = {"{e} Theater", "The {e} Cinema"};
        p["church"] = {"{a} Church", "{a} Chapel"};
        p["cathedral"] = {"Cathedral of {a}"};
        p["school"] = {"{p} Elementary", "{nature} Middle School", "{s} High School"};
        p["university"] = {"{s} University", "{e} College"};
        p["hospital"] = {"{a} Hospital", "{s} General Hospital"};
        p["police"] = {"Police Precinct {#}", "Police Station {#}"};
        p["fireStation"] = {"Fire Station {#}"};
        p["townhall"] = {"City Hall", "{e} Town Hall"};
        p["government"] = {"Government Office {#}", "{s} Municipal Building"};
        p["courthouse"] = {"{s} Courthouse"};
        p["military"] = {"Fort {s}", "Camp {s}"};
        p["postOffice"] = {"Post Office {#}"};
        p["library"] = {"{p} Library"};
        p["museum"] = {"{e} Museum", "{p} Museum"};
        p["theatre"] = {"The {e} Playhouse", "{s} Theatre"};
        p["stadium"] = {"{e} Stadium", "{s} Field"};
        p["sports"] = {"{s} Sports Ground", "{e} Park"};
        p["port"] = {"Port {s}", "{e} Docks"};
        p["ferryTerminal"] = {"{e} Ferry Terminal"};
        p["marina"] = {"{e} Marina"};
        p["airport"] = {"{p} International Airport", "{s} Field"};
        p["airstrip"] = {"{s} Airstrip"};
        p["busStation"] = {"{e} Bus Depot"};
        p["cemetery"] = {"{e} Cemetery", "{a} Cemetery"};
        p["park"] = {"{e} Park", "{s} Park"};
        p["office"] = {"{s} & {s} Offices", "{s} Building"};
        p["warehouse"] = {"{s} Storage", "{e} Warehouse"};
        p["factory"] = {"{s} Industries", "{e} Works"};
        p["packingPlant"] = {"{e} Packing Co.", "{s} Packers"};
        p["coldStorage"] = {"{e} Cold Storage"};
        p["truckYard"] = {"{s} Trucking", "{e} Freight"};
        p["farm"] = {"{e} Farm", "{s} Ranch"};
        p["greenhouse"] = {"{e} Greenhouses", "{s} Growers"};
        p["lighthouse"] = {"{e} Lighthouse"};
        p["shop"] = {"{e} Store", "{s} & Sons"};
        p["building"] = {"The {e} Building", "{s} Tower"};
        p["apartments"] = {"{e} Apartments", "{s} Court"};
        p["mansion"] = {"{s} House", "{e} Manor"};
        p["shipping"] = {"{e} Lines", "{s} Shipping", "{e} Container Line"};
        p["cruiseLine"] = {"{e} Cruises", "{s} Cruise Line"};
        p["cruiseTerminal"] = {"{e} Cruise Terminal", "{s} Pier"};
        p["customs"] = {"{e} Customs House", "{s} Customs Post"};
        p["mausoleum"] = {"{s} Family", "{s}-{s} Family", "{s} Family Chapel"};
        x.business = {"{s} & Co.", "{e} Store", "{g}'s"};
        auto& k = x.kinds;
        k[Kind::Town] = {"{n}", "Port {s}", "{s}ville", "{s} Springs", "New {s}"};
        k[Kind::River] = {"{s} River", "{e} River"};
        k[Kind::Stream] = {"{e} Creek", "{s} Brook"};
        k[Kind::Lake] = {"Lake {s}", "{e} Lake"};
        k[Kind::Reservoir] = {"{s} Reservoir"};
        k[Kind::Dam] = {"{s} Dam"};
        k[Kind::Island] = {"{s} Island", "{e} Island"};
        k[Kind::Beach] = {"{e} Beach", "{s} Beach"};
        k[Kind::Bay] = {"{s} Bay", "{e} Cove"};
        k[Kind::Mountain] = {"Mount {s}", "{e} Peak"};
        k[Kind::Park] = {"{e} Park", "{s} Park"};
        k[Kind::Landmark] = {"{e} Monument", "{s} Memorial"};
        return x;
    }();
    return v;
}

const Vocab& vocab(std::string_view culture) { return vocabularyOf(culture) == "mexican" ? mexicanVocab() : englishVocab(); }

/** The program a kind names its things as when it has no patterns of its own. */
const char* kindProgram(Kind k)
{
    switch (k)
    {
    case Kind::Port: return "port";
    case Kind::Airport: return "airport";
    case Kind::Cemetery: return "cemetery";
    case Kind::School: return "school";
    case Kind::Hospital: return "hospital";
    case Kind::Government: return "government";
    case Kind::Police: return "police";
    case Kind::Religious: return "church";
    case Kind::Stadium: return "stadium";
    case Kind::Market: return "market";
    case Kind::Building: return "building";
    default: return "";
    }
}

std::string expand(const Vocab& v, std::string_view pattern, uint64_t k)
{
    std::string out;
    int slot = 0;
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] != '{')
        {
            out += pattern[i];
            continue;
        }
        const size_t end = pattern.find('}', i);
        const std::string_view tag = pattern.substr(i + 1, end - i - 1);
        const uint64_t h = hash64(k ^ (0xA24BAED4963EE407ull * uint64_t(++slot)));
        if (tag == "g")
            out += pick(v.given, h);
        else if (tag == "s")
            out += pick(v.surnames, h);
        else if (tag == "e")
            out += pick(v.evocative, h);
        else if (tag == "n" || tag == "nature")
            out += pick(v.nature, h);
        else if (tag == "a")
            out += pick(v.saints, h);
        else if (tag == "p")
            out += pick(v.persons, h);
        else if (tag == "#")
            out += std::to_string(1 + int(h % 48));
        i = end;
    }
    // "San San Rafael de ..." and "El El Faro": a pattern's own article or title meets the word's.
    for (const char* dup : {"San San ", "San Santa ", "El El ", "El La ", "El Las ", "El Los "})
        if (out.starts_with(dup))
            out.erase(0, std::string_view(dup).find(' ') + 1);
    return out;
}
} // namespace

std::vector<std::string> cultures()
{
    std::vector<std::string> out;
    for (const Culture& c : all())
        out.emplace_back(c.name);
    return out;
}

bool hasCulture(std::string_view culture)
{
    return std::ranges::any_of(all(), [&](const Culture& c) { return culture == c.name; });
}

void requireCulture(std::string_view culture) { cultureNamed(culture); }

std::string cultureLabel(std::string_view culture)
{
    static const std::pair<const char*, const char*> kLabels[] = {{"oldEnglish", "Old English"}, {"latin", "Latin"}, {"french", "French"},
                                                                 {"spanish", "Spanish"},        {"usGrid", "US Grid"}, {"sector", "Sector Codes"},
                                                                 {"mexican", "Mexican"}};
    for (const auto& [k, v] : kLabels)
        if (culture == k)
            return v;
    return std::string(culture);
}

std::string vocabularyOf(std::string_view culture)
{
    for (const Culture& c : all())
        if (culture == c.name)
            return c.vocabulary;
    return "english";
}

std::string cultureForLocation(double lat, double lon, std::string* why)
{
    struct Box
    {
        const char* culture;
        const char* place;
        double s, w, n, e;
    };
    // Rough national boxes, first match wins: enough to choose words, not to draw borders.
    static const Box boxes[] = {
        {"mexican", "Mexico", 14.3, -118.5, 32.8, -86.5},
        {"usGrid", "the United States", 24.0, -125.0, 49.5, -66.0},
        {"usGrid", "Canada", 41.6, -141.0, 72.0, -52.0},
        {"spanish", "Spain", 35.9, -9.5, 43.9, 3.4},
        {"french", "France", 41.3, -5.2, 51.2, 9.6},
        {"oldEnglish", "Great Britain and Ireland", 49.8, -10.7, 60.9, 1.8},
        {"mexican", "Central and South America", -56.0, -92.0, 14.3, -34.0},
    };
    for (const Box& b : boxes)
        if (lat >= b.s && lat <= b.n && lon >= b.w && lon <= b.e)
        {
            if (why)
                *why = std::string("the data lies in ") + b.place;
            return b.culture;
        }
    if (why)
        *why = "no culture is set for the data's location; oldEnglish used";
    return "oldEnglish";
}

const std::vector<std::string>& programs()
{
    static const std::vector<std::string> p = [] {
        std::vector<std::string> out;
        for (const auto& [name, patterns] : mexicanVocab().programs)
            out.push_back(name);
        return out;
    }();
    return p;
}

std::string streetName(std::string_view culture, int cls, uint64_t h, int attempt)
{
    const Culture& c = cultureNamed(culture);
    const size_t ci = size_t(std::clamp(cls, 0, 4));
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
    {
        const std::string word = pick(c.classWords[ci], k);
        const int n = 1 + int(hash64(k) % 99);
        if (cls <= 0)
            return word + " " + std::to_string(n);
        return word + " " + char('A' + int(hash64(k ^ 7) % 26)) + std::to_string(n);
    }
    const std::string root = pick(c.roots, k), word = pick(c.classWords[ci], hash64(k ^ 0x51));
    return c.wordFirst ? word + " " + root : root + " " + word;
}

std::string districtName(std::string_view culture, uint64_t h, int attempt)
{
    const Culture& c = cultureNamed(culture);
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
        return "Sector " + std::to_string(1 + int(k % 40)) + char('A' + int(hash64(k) % 26));
    std::string root = pick(c.districtRoots, k);
    if (!c.districtEnds.empty())
        root += pick(c.districtEnds, hash64(k ^ 0x33));
    return c.districtPrefix + root;
}

std::string placeName(std::string_view culture, Kind kind, std::string_view program, uint64_t key, int attempt)
{
    requireCulture(culture);
    if (kind == Kind::Street)
        return streetName(culture, 2, key, attempt);
    if (kind == Kind::District)
        return districtName(culture, key, attempt);
    const Vocab& v = vocab(culture);
    const uint64_t k = hash64(key + uint64_t(attempt) * 0x9E3779B97F4A7C15ull + uint64_t(kind) * 0xD6E8FEB86659FD93ull);
    const Words* patterns = nullptr;
    if (!program.empty())
        if (auto it = v.programs.find(program); it != v.programs.end())
            patterns = &it->second;
    if (!patterns)
        if (auto it = v.kinds.find(kind); it != v.kinds.end())
            patterns = &it->second;
    if (!patterns)
        if (auto it = v.programs.find(kindProgram(kind)); it != v.programs.end())
            patterns = &it->second;
    if (!patterns)
        patterns = &v.business;
    return expand(v, pick(*patterns, k), hash64(k ^ 0x5bd1e995));
}
} // namespace lila::names
