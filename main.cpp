//#define DEBUG_CONSOLE // Uncomment this if you want a debug console to start. You can use the Console class to print. You can use Console::inStrings to get input.

#include <4dm.h>

using namespace fdm;

#include "strUtils.h"

// Initialize the DLLMain
initDLL

#include "JSONData.h"

/*
clientA->(server)->clientB:
1. clientA->(server)
{
	"target": "EntityPlayer UUID"|("all"|null)|"!EntityPlayer UUID",				// specific|all|all_except
	"packet": "name",
	"data": json
}
2. (server)->clientB
{
	"packet": "name",
	"data": json,
	"from":
	{
		"uuid": "EntityPlayer UUID",
		"name": "Player Name"
	}
}
*/

/*
client->server
{
	"target": "server"
	"packet": "name",
	"data": json
}
*/

/*
server->client
{
	"from": "server"
	"packet": "name",
	"data": json
}
*/

void cschandleJsonMessage(WorldClient* world, Player* player, const nlohmann::json& data, const stl::string& packet, const stl::uuid& from, const stl::string& fromName);
void cshandleJsonMessage(WorldServer* world, double dt, const nlohmann::json& data, const stl::string& packet, uint32_t client);
void schandleJsonMessage(WorldClient* world, Player* player, const nlohmann::json& data, const stl::string& packet);
$hook(void, WorldServer, handleMessage, const Connection::InMessage& message, double dt)
{
	if (message.getPacketType() == JSONData::C_JSON)
	{
		if (!self->players.contains(message.getClient()))
			return;

		std::string data = message.getStrData();
		if (data.empty()) return;

		nlohmann::json j;
		try
		{
			j = nlohmann::json::from_cbor(data);
		}
		catch (const nlohmann::json::exception& e)
		{
			Console::printLine(
				Console::Mode(Console::GREEN, Console::BRIGHT),
				"JSONData::WorldServer::handleMessage:",
				Console::Mode(Console::RED, Console::BRIGHT),
				" Exception: Couldn't parse JSON: ", e.what());
			return;
		}

		if (!j.contains("data") || !j.contains("target") || !j.contains("packet")) return;

		if (j.at("target").is_string() && j["target"] == "server")
		{
			cshandleJsonMessage(self, dt, j["data"], j["packet"].get<std::string>(), message.getClient());
			return;
		}

		Connection::Server::MessageTarget target = Connection::Server::TARGET_ALL_CLIENTS;
		uint32_t targetHandle = 0;
		{
			auto& t = j["target"];
			if (t.is_string() && t.size() > 2)
			{
				std::string tStr = t.get<std::string>();
				trim(tStr);
				toLower(tStr);

				if (tStr == "all")
				{
					target = Connection::Server::TARGET_ALL_CLIENTS;
					targetHandle = 0;
				}
				else
				{
					if (tStr.starts_with('!'))
					{
						target = Connection::Server::TARGET_ALL_CLIENTS_EXCEPT;
						tStr = tStr.substr(1);
					}
					else
					{
						target = Connection::Server::TARGET_SPECIFIC_CLIENT;
					}

					stl::uuid uuid{};
					try
					{
						uuid = uuid(tStr);
					}
					catch (const std::runtime_error& e)
					{
						//Console::printLine(Console::Mode(Console::RED, Console::BRIGHT), "invalid uuid");
						return;
					}
					
					if (self->entityPlayerIDs.contains(uuid))
					{
						auto& pl = self->entityPlayerIDs.at(uuid);
						targetHandle = pl->handle;
					}
					else
					{
						target = Connection::Server::TARGET_ALL_CLIENTS;
						targetHandle = 0;
					}
				}
			}
			j.erase("target");
		}
		auto& player = self->players.at(message.getClient());
		
		j["from"] = nlohmann::json
		{
			{ "uuid", std::string(stl::uuid::to_string(player.player->EntityPlayerID)) },
			{ "name", std::string(player.displayName) }
		};
		if (!j.contains("packet") || !j.at("packet").is_string() || !j.contains("data"))
			return;
		std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
		if (msgData.size() > 1024 * 1024) // 1mb max
			return;
		self->server.sendMessage(Connection::OutMessage{ JSONData::S_JSON, msgData.data(), msgData.size() }, target, targetHandle, true);
		return;
	}
	return original(self, message, dt);
}
$hook(void, WorldClient, handleMessage, const Connection::InMessage& message, Player* player)
{
	if (message.getPacketType() == JSONData::S_JSON)
	{
		std::string data = message.getStrData();
		if (data.empty()) return;

		nlohmann::json j;
		try
		{
			j = nlohmann::json::from_cbor(data);
		}
		catch (const nlohmann::json::exception& e)
		{
			Console::printLine(
				Console::Mode(Console::GREEN, Console::BRIGHT),
				"JSONData::WorldClient::handleMessage:",
				Console::Mode(Console::RED, Console::BRIGHT),
				" Exception: Couldn't parse JSON: ", e.what());
			return;
		}

		if (!j.contains("packet") || !j.contains("data") || !j.contains("from") || !j.at("packet").is_string())
			return;

		if (j.at("from").is_object() && j.at("from").contains("name") && j.at("from").contains("uuid")
			&& j.at("from").at("uuid").is_string() && j.at("from").at("name").is_string())
		{
			auto& from = j["from"];
			std::string fromUUID = from["uuid"];
			std::string fromName = from["name"];

			stl::uuid uuid{};
			try
			{
				uuid = uuid(fromUUID);
			}
			catch (const std::runtime_error& e)
			{
				//Console::printLine(Console::Mode(Console::RED, Console::BRIGHT), "invalid uuid");
				return;
			}

			cschandleJsonMessage(self, player, j["data"], j["packet"].get<std::string>(), uuid, fromName);
		}
		else if (j.at("from").is_string() && j["from"] == "server")
		{
			schandleJsonMessage(self, player, j["data"], j["packet"].get<std::string>());
		}

		return;
	}
	original(self, message, player);
}

inline std::unordered_map<stl::string, std::set<JSONData::CSCPacketCallback>> cscPacketCallbacks{};
inline std::unordered_map<stl::string, std::set<JSONData::SCPacketCallback>> scPacketCallbacks{};
inline std::unordered_map<stl::string, std::set<JSONData::CSPacketCallback>> csPacketCallbacks{};

extern "C" __declspec(dllexport) inline void addPacketCallback(const stl::string& packet, JSONData::CSCPacketCallback callback)
{
	if (fdm::isServer()) return;
	if (!cscPacketCallbacks[packet].contains(callback))
		cscPacketCallbacks[packet].insert(callback);
}
extern "C" __declspec(dllexport) inline void removePacketCallback(const stl::string& packet, JSONData::CSCPacketCallback callback)
{
	if (fdm::isServer()) return;
	if (cscPacketCallbacks[packet].contains(callback))
		cscPacketCallbacks[packet].erase(callback);
}
extern "C" __declspec(dllexport) inline void SCaddPacketCallback(const stl::string& packet, JSONData::SCPacketCallback callback)
{
	if (fdm::isServer()) return;
	if (!scPacketCallbacks[packet].contains(callback))
		scPacketCallbacks[packet].insert(callback);
}
extern "C" __declspec(dllexport) inline void SCremovePacketCallback(const stl::string& packet, JSONData::SCPacketCallback callback)
{
	if (fdm::isServer()) return;
	if (scPacketCallbacks[packet].contains(callback))
		scPacketCallbacks[packet].erase(callback);
}
extern "C" __declspec(dllexport) inline void CSaddPacketCallback(const stl::string& packet, JSONData::CSPacketCallback callback)
{
	if (!fdm::isServer()) return;
	if (!csPacketCallbacks[packet].contains(callback))
		csPacketCallbacks[packet].insert(callback);
}
extern "C" __declspec(dllexport) inline void CSremovePacketCallback(const stl::string& packet, JSONData::CSPacketCallback callback)
{
	if (!fdm::isServer()) return;
	if (csPacketCallbacks[packet].contains(callback))
		csPacketCallbacks[packet].erase(callback);
}
extern "C" __declspec(dllexport) inline void sendPacketAll(WorldClient* world, const stl::string& packet, const nlohmann::json& data, bool reliable = true)
{
	if (fdm::isServer()) return;
	if (!world) return;
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "target", "all" }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->client->sendMessage(Connection::OutMessage{ JSONData::C_JSON, msgData.data(), msgData.size() }, reliable);
}
extern "C" __declspec(dllexport) inline void sendPacketSpecific(WorldClient* world, const stl::string& packet, const nlohmann::json& data, const stl::uuid& target, bool reliable = true)
{
	if (fdm::isServer()) return;
	if (!world) return;
	stl::string uuidStr = stl::uuid::to_string(target);
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "target", uuidStr }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->client->sendMessage(Connection::OutMessage{ JSONData::C_JSON, msgData.data(), msgData.size() }, reliable);
}
extern "C" __declspec(dllexport) inline void sendPacketAllExcept(WorldClient* world, const stl::string& packet, const nlohmann::json& data, const stl::uuid& target, bool reliable = true)
{
	if (fdm::isServer()) return;
	if (!world) return;
	stl::string uuidStr = stl::uuid::to_string(target);
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "target", std::format("!{}", uuidStr) }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->client->sendMessage(Connection::OutMessage{ JSONData::C_JSON, msgData.data(), msgData.size() }, reliable);
}
extern "C" __declspec(dllexport) inline void sendPacketServer(WorldClient* world, const stl::string& packet, const nlohmann::json& data, bool reliable = true)
{
	if (fdm::isServer()) return;
	if (!world) return;
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "target", "server" }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->client->sendMessage(Connection::OutMessage{ JSONData::C_JSON, msgData.data(), msgData.size() }, reliable);
}
extern "C" __declspec(dllexport) inline void sendPacketClient(WorldServer* world, const stl::string& packet, const nlohmann::json& data, uint32_t client, bool reliable = true)
{
	if (!fdm::isServer()) return;
	if (!world) return;
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "from", "server" }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->server.sendMessage(Connection::OutMessage{ JSONData::S_JSON, msgData.data(), msgData.size() }, fdm::Connection::Server::TARGET_SPECIFIC_CLIENT, client, reliable);
}
extern "C" __declspec(dllexport) inline void broadcastPacket(WorldServer* world, const stl::string& packet, const nlohmann::json& data, bool reliable = true)
{
	if (!fdm::isServer()) return;
	if (!world) return;
	nlohmann::json j
	{
		{ "packet", packet },
		{ "data", data },
		{ "from", "server" }
	};
	std::vector<uint8_t> msgData = nlohmann::json::to_cbor(j);
	if (msgData.size() > 1024 * 1024) // 1mb max
		return;
	world->server.sendMessage(Connection::OutMessage{ JSONData::S_JSON, msgData.data(), msgData.size() }, fdm::Connection::Server::TARGET_ALL_CLIENTS, 0, reliable);
}

void cschandleJsonMessage(WorldClient* world, Player* player, const nlohmann::json& data, const stl::string& packet, const stl::uuid& from, const stl::string& fromName)
{
	if (cscPacketCallbacks.contains(packet))
	{
		const auto& funcs = cscPacketCallbacks.at(packet);
		for (auto& f : funcs)
		{
			f(world, player, data, from, fromName);
		}
	}
}

void cshandleJsonMessage(WorldServer* world, double dt, const nlohmann::json& data, const stl::string& packet, uint32_t client)
{
	if (csPacketCallbacks.contains(packet))
	{
		const auto& funcs = csPacketCallbacks.at(packet);
		for (auto& f : funcs)
		{
			f(world, dt, data, client);
		}
	}
}

void schandleJsonMessage(WorldClient* world, Player* player, const nlohmann::json& data, const stl::string& packet)
{
	if (scPacketCallbacks.contains(packet))
	{
		const auto& funcs = scPacketCallbacks.at(packet);
		for (auto& f : funcs)
		{
			f(world, player, data);
		}
	}
}
