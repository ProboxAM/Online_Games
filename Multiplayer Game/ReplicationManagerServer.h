#pragma once
#include <unordered_map>

// TODO(you): World state replication lab session
class ReplicationManagerServer
{
public:

	void create(uint32 networkId);
	void update(uint32 networkId);
	void destroy(uint32 networkId);

	void write(OutputMemoryStream &packet);

	std::unordered_map<uint32, ReplicationCommand> commands;
};