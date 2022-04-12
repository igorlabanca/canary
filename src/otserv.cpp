/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include <fstream>

#ifdef OS_WINDOWS
	#include "conio.h"
#endif

#include "declarations.hpp"
#include "utils/definitions.h"
#include "creatures/combat/spells.h"
#include "database/databasemanager.h"
#include "database/databasetasks.h"
#include "game/game.h"
#include "game/scheduling/scheduler.h"
#include "io/iomarket.h"
#include "lua/creature/events.h"
#include "lua/modules/modules.h"
#include "lua/scripts/lua_environment.hpp"
#include "lua/scripts/scripts.h"
#include "security/rsa.h"
#include "server/network/protocol/protocollogin.h"
#include "server/network/protocol/protocolstatus.h"
#include "server/network/webhook/webhook.h"
#include "server/server.h"

#if __has_include("gitmetadata.h")
	#include "gitmetadata.h"
#endif

DatabaseTasks g_databaseTasks;
Dispatcher g_dispatcher;
Scheduler g_scheduler;

extern Events* g_events;
extern Imbuements* g_imbuements;
extern LuaEnvironment g_luaEnvironment;
extern Modules* g_modules;
Monsters g_monsters;
Npcs g_npcs;
Vocations g_vocations;
extern Scripts* g_scripts;
RSA2 g_RSA;

std::mutex g_loaderLock;
std::condition_variable g_loaderSignal;
std::unique_lock<std::mutex> g_loaderUniqueLock(g_loaderLock);

/**
 *It is preferable to keep the close button off as it closes the server without saving (this can cause the player to lose items from houses and others informations, since windows automatically closes the process in five seconds, when forcing the close)
 * Choose to use "CTROL + C" or "CTROL + BREAK" for security close
 * To activate/desactivate window;
 * \param MF_GRAYED Disable the "x" (force close) button
 * \param MF_ENABLED Enable the "x" (force close) button
*/
void toggleForceCloseButton() {
	#ifdef OS_WINDOWS
	HWND hwnd = GetConsoleWindow();
	HMENU hmenu = GetSystemMenu(hwnd, FALSE);
	EnableMenuItem(hmenu, SC_CLOSE, MF_GRAYED);
	#endif
}

std::string getCompiler() {
	std::string compiler;
	#if defined(__clang__)
		return compiler = "Clang++ " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__) +"";
	#elif defined(_MSC_VER)
		return compiler = "Microsoft Visual Studio " + std::to_string(_MSC_VER) +"";
	#elif defined(__GNUC__)
		return compiler = "G++ " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__) +"";
	#else
		return compiler = "unknown";
	#endif
}

std::string getCppVersion() {
	std::string cppVersion;
	// c++ 20 or higher still doesn't have a definitive version
	if (__cplusplus > 201703L) {
		return cppVersion = "C++20 or higher";
	}
	else if (__cplusplus == 201703L) {
		return cppVersion = "C++17";
	}
	else if (__cplusplus == 201402L) {
		return cppVersion = "C++14";
	}
	else if (__cplusplus == 201103L) {
		return cppVersion = "C++11";
	}
	else if (__cplusplus == 199711L) {
		return cppVersion = "C++98";
	}
	else {
		return "C++ unknown";
	}
}

std::string getPlatform() {
	std::string platform;
	#if defined(__amd64__) || defined(_M_X64)
		return platform = "x64";
	#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
		return platform = "x86";
	#elif defined(__arm__)
		return platform = "ARM";
	#else
		return platform = "unknown";
	#endif
}

void startupErrorMessage() {
	SPDLOG_ERROR("The program will close after pressing the enter key...");
	g_loaderSignal.notify_all();
	getchar();
	exit(-1);
}

void mainLoader(int argc, char* argv[], ServiceManager* servicer);

void badAllocationHandler() {
	// Use functions that only use stack allocation
	SPDLOG_ERROR("Allocation failed, server out of memory, "
                 "decrease the size of your map or compile in 64 bits mode");
	getchar();
	exit(-1);
}

void initGlobalScopes() {
	g_scripts = new Scripts();
	g_modules = new Modules();
	g_events = new Events();
	g_imbuements = new Imbuements();
}

void modulesLoadHelper(bool loaded, std::string moduleName) {
	SPDLOG_INFO("Loading {}", moduleName);
	if (!loaded) {
		SPDLOG_ERROR("Cannot load: {}", moduleName);
		startupErrorMessage();
	}
}

void loadModules() {
	modulesLoadHelper(g_configManager().load(),
		"config.lua");

	SPDLOG_INFO("Server protocol: {}",
		g_configManager().getString(CLIENT_VERSION_STR));

	// set RSA key
	try {
		g_RSA.loadPEM("key.pem");
	} catch(const std::exception& e) {
		SPDLOG_ERROR(e.what());
		startupErrorMessage();
	}

	// Database
	SPDLOG_INFO("Establishing database connection... ");
	if (!Database::getInstance().connect()) {
		SPDLOG_ERROR("Failed to connect to database!");
		startupErrorMessage();
	}
	SPDLOG_INFO("MySQL Version: {}", Database::getClientVersion());

	// Run database manager
	SPDLOG_INFO("Running database manager...");
	if (!DatabaseManager::isDatabaseSetup()) {
		SPDLOG_ERROR("The database you have specified in config.lua is empty, "
			"please import the schema.sql to your database.");
		startupErrorMessage();
	}

	g_databaseTasks.start();
	DatabaseManager::updateDatabase();

	if (g_configManager().getBoolean(OPTIMIZE_DATABASE)
			&& !DatabaseManager::optimizeTables()) {
		SPDLOG_INFO("No tables were optimized");
	}

	modulesLoadHelper((Item::items.loadFromOtb("data/items/items.otb") == ERROR_NONE),
		"items.otb");
	modulesLoadHelper(Item::items.loadFromXml(),
		"items.xml");
	modulesLoadHelper(Scripts::getInstance().loadScriptSystems(),
		"script systems");

	// Lua Env
	modulesLoadHelper((g_luaEnvironment.loadFile("data/global.lua") == 0),
		"data/global.lua");
	modulesLoadHelper((g_luaEnvironment.loadFile("data/stages.lua") == 0),
		"data/stages.lua");
	modulesLoadHelper((g_luaEnvironment.loadFile("data/startup/startup.lua") == 0),
		"data/startup/startup.lua");
	modulesLoadHelper((g_luaEnvironment.loadFile("data/npclib/load.lua") == 0),
		"data/npclib/load.lua");

	modulesLoadHelper(g_scripts->loadScripts("scripts/lib", true, false),
		"data/scripts/libs");
	modulesLoadHelper(g_vocations.loadFromXml(),
		"data/XML/vocations.xml");
	modulesLoadHelper(g_game().loadScheduleEventFromXml(),
		"data/XML/events.xml");
	modulesLoadHelper(Outfits::getInstance().loadFromXml(),
		"data/XML/outfits.xml");
	modulesLoadHelper(Familiars::getInstance().loadFromXml(),
		"data/XML/familiars.xml");
	modulesLoadHelper(g_imbuements->loadFromXml(),
		"data/XML/imbuements.xml");
	modulesLoadHelper(g_modules->loadFromXml(),
		"data/modules/modules.xml");
	modulesLoadHelper(g_events->loadFromXml(),
		"data/events/events.xml");
	modulesLoadHelper(g_scripts->loadScripts("scripts", false, false),
		"data/scripts");
	modulesLoadHelper(g_scripts->loadScripts("monster", false, false),
		"data/monster");
	modulesLoadHelper(g_scripts->loadScripts("npclua", false, false),
		"data/npclua");

	g_game().loadBoostedCreature();
}

#ifndef UNIT_TESTING
int main(int argc, char* argv[]) {
#ifdef DEBUG_LOG
	SPDLOG_DEBUG("[CANARY] SPDLOG LOG DEBUG ENABLED");
	spdlog::set_pattern("[%Y-%d-%m %H:%M:%S.%e] [file %@] [func %!] [thread %t] [%^%l%$] %v ");
#else
	spdlog::set_pattern("[%Y-%d-%m %H:%M:%S.%e] [%^%l%$] %v ");
#endif
	// Toggle force close button enabled/disabled
	toggleForceCloseButton();

	// Setup bad allocation handler
	std::set_new_handler(badAllocationHandler);

	ServiceManager serviceManager;

	g_dispatcher.start();
	g_scheduler.start();

	g_dispatcher.addTask(createTask(std::bind(mainLoader, argc, argv,
												&serviceManager)));

	g_loaderSignal.wait(g_loaderUniqueLock);

	if (serviceManager.is_running()) {
		SPDLOG_INFO("{} {}", g_configManager().getString(SERVER_NAME),
                    "server online!");
		serviceManager.run();
	} else {
		SPDLOG_ERROR("No services running. The server is NOT online!");
		g_databaseTasks.shutdown();
		g_dispatcher.shutdown();
		exit(-1);
	}

	g_scheduler.join();
	g_databaseTasks.join();
	g_dispatcher.join();
	return 0;
}
#endif

void mainLoader(int, char*[], ServiceManager* services) {
	// dispatcher thread
	g_game().setGameState(GAME_STATE_STARTUP);

	srand(static_cast<unsigned int>(OTSYS_TIME()));
#ifdef _WIN32
#ifdef UNICODE
SetConsoleTitle(reinterpret_cast<LPCWSTR>(STATUS_SERVER_NAME));
#else
SetConsoleTitle(reinterpret_cast<LPCSTR>(STATUS_SERVER_NAME));
#endif  // !UNICODE
#endif  // _WIN32
#if defined(GIT_RETRIEVED_STATE) && GIT_RETRIEVED_STATE
	SPDLOG_INFO("{} - Version [{}] dated [{}]",
                STATUS_SERVER_NAME, STATUS_SERVER_VERSION, GIT_COMMIT_DATE_ISO8601);
	#if GIT_IS_DIRTY
	SPDLOG_WARN("DIRTY - NOT OFFICIAL RELEASE");
	#endif
#else
	SPDLOG_INFO("{} - Version {}", STATUS_SERVER_NAME, STATUS_SERVER_VERSION);
#endif

	// Increment compiller information
	SPDLOG_INFO("Compiled with {}, linked with standard {}", getCompiler(), getCppVersion());
	// Increment platform information
	SPDLOG_INFO("Compiled on {} {} for platform {}\n", __DATE__, __TIME__, getPlatform());

#if defined(LUAJIT_VERSION)
	SPDLOG_INFO("Linked with {} for Lua support", LUAJIT_VERSION);
#endif

	SPDLOG_INFO("A server developed by: {}", STATUS_SERVER_DEVELOPERS);
	SPDLOG_INFO("Visit our website for updates, support, and resources: "
		"https://docs.opentibiabr.org/");

	// Check if config.lua or config.lua.dist exist
	std::ifstream c_test("./config.lua");
	if (!c_test.is_open()) {
		std::ifstream config_lua_dist("./config.lua.dist");
		if (config_lua_dist.is_open()) {
			SPDLOG_INFO("Copying config.lua.dist to config.lua");
			std::ofstream config_lua("config.lua");
			config_lua << config_lua_dist.rdbuf();
			config_lua.close();
			config_lua_dist.close();
		}
	} else {
		c_test.close();
	}

	// Init and load modules
	initGlobalScopes();
	loadModules();

#ifdef _WIN32
	const std::string& defaultPriority = g_configManager().getString(DEFAULT_PRIORITY);
	if (strcasecmp(defaultPriority.c_str(), "high") == 0) {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	} else if (strcasecmp(defaultPriority.c_str(), "above-normal") == 0) {
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	}
#endif

	std::string worldType = asLowerCaseString(g_configManager().getString(WORLD_TYPE));
	if (worldType == "pvp") {
		g_game().setWorldType(WORLD_TYPE_PVP);
	} else if (worldType == "no-pvp") {
		g_game().setWorldType(WORLD_TYPE_NO_PVP);
	} else if (worldType == "pvp-enforced") {
		g_game().setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		SPDLOG_ERROR("Unknown world type: {}, valid world types are: pvp, no-pvp "
			"and pvp-enforced", g_configManager().getString(WORLD_TYPE));
		startupErrorMessage();
	}

	SPDLOG_INFO("World type set as {}", asUpperCaseString(worldType));

	SPDLOG_INFO("Loading map...");
	if (!g_game().loadMainMap(g_configManager().getString(MAP_NAME))) {
		SPDLOG_ERROR("Failed to load map");
		startupErrorMessage();
	}

	// If "mapCustomEnabled" is true on config.lua, then load the custom map
	if (g_configManager().getBoolean(TOGGLE_MAP_CUSTOM)) {
		SPDLOG_INFO("Loading custom map...");
		if (!g_game().loadCustomMap(g_configManager().getString(MAP_CUSTOM_NAME))) {
			SPDLOG_ERROR("Failed to load custom map");
			startupErrorMessage();
		}
	}

	SPDLOG_INFO("Initializing gamestate...");
	g_game().setGameState(GAME_STATE_INIT);

	// Game client protocols
	services->add<ProtocolGame>(static_cast<uint16_t>(g_configManager().getNumber(GAME_PORT)));
	services->add<ProtocolLogin>(static_cast<uint16_t>(g_configManager().getNumber(LOGIN_PORT)));
	// OT protocols
	services->add<ProtocolStatus>(static_cast<uint16_t>(g_configManager().getNumber(STATUS_PORT)));

	RentPeriod_t rentPeriod;
	std::string strRentPeriod = asLowerCaseString(g_configManager().getString(HOUSE_RENT_PERIOD));

	if (strRentPeriod == "yearly") {
		rentPeriod = RENTPERIOD_YEARLY;
	} else if (strRentPeriod == "weekly") {
		rentPeriod = RENTPERIOD_WEEKLY;
	} else if (strRentPeriod == "monthly") {
		rentPeriod = RENTPERIOD_MONTHLY;
	} else if (strRentPeriod == "daily") {
		rentPeriod = RENTPERIOD_DAILY;
	} else {
		rentPeriod = RENTPERIOD_NEVER;
	}

	g_game().map.houses.payHouses(rentPeriod);

	IOMarket::checkExpiredOffers();
	IOMarket::getInstance().updateStatistics();

	SPDLOG_INFO("Loaded all modules, server starting up...");

#ifndef _WIN32
	if (getuid() == 0 || geteuid() == 0) {
		SPDLOG_WARN("{} has been executed as root user, "
                    "please consider running it as a normal user",
                    STATUS_SERVER_NAME);
	}
#endif

	g_game().start(services);
	g_game().setGameState(GAME_STATE_NORMAL);

	webhook_init();

	std::string url = g_configManager().getString(DISCORD_WEBHOOK_URL);
	webhook_send_message("Server is now online", "Server has successfully started.", WEBHOOK_COLOR_ONLINE, url);

	g_loaderSignal.notify_all();
}
