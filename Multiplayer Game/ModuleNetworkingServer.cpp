#include "ModuleNetworkingServer.h"



//////////////////////////////////////////////////////////////////////
// ModuleNetworkingServer public methods
//////////////////////////////////////////////////////////////////////

void ModuleNetworkingServer::setListenPort(int port)
{
	listenPort = port;
}

void ModuleNetworkingServer::DeliverySuccess(DeliveryManager* manager)
{
	LOG("Delivery was successfull");
}


//////////////////////////////////////////////////////////////////////
// ModuleNetworking virtual methods
//////////////////////////////////////////////////////////////////////

void ModuleNetworkingServer::onStart()
{
	if (!createSocket()) return;

	// Reuse address
	int enable = 1;
	int res = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(int));
	if (res == SOCKET_ERROR) {
		reportError("ModuleNetworkingServer::start() - setsockopt");
		disconnect();
		return;
	}

	// Create and bind to local address
	if (!bindSocketToPort(listenPort)) {
		return;
	}

	state = ServerState::Listening;

	secondsSinceSendPingPacket = 0.0f;
}

void ModuleNetworkingServer::onGui()
{
	if (ImGui::CollapsingHeader("ModuleNetworkingServer", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("Connection checking info:");
		ImGui::Text(" - Ping interval (s): %f", PING_INTERVAL_SECONDS);
		ImGui::Text(" - Disconnection timeout (s): %f", DISCONNECT_TIMEOUT_SECONDS);

		ImGui::Separator();

		if (state == ServerState::Listening)
		{
			int count = 0;

			for (int i = 0; i < MAX_CLIENTS; ++i)
			{
				if (clientProxies[i].connected)
				{
					ImGui::Text("CLIENT %d", count++);
					ImGui::Text(" - address: %d.%d.%d.%d",
						clientProxies[i].address.sin_addr.S_un.S_un_b.s_b1,
						clientProxies[i].address.sin_addr.S_un.S_un_b.s_b2,
						clientProxies[i].address.sin_addr.S_un.S_un_b.s_b3,
						clientProxies[i].address.sin_addr.S_un.S_un_b.s_b4);
					ImGui::Text(" - port: %d", ntohs(clientProxies[i].address.sin_port));
					ImGui::Text(" - name: %s", clientProxies[i].name.c_str());
					ImGui::Text(" - id: %d", clientProxies[i].clientId);
					if (clientProxies[i].gameObject != nullptr)
					{
						ImGui::Text(" - gameObject net id: %d", clientProxies[i].gameObject->networkId);
						ImGui::Text(" - gameObject position: %f %f", clientProxies[i].gameObject->position.x, clientProxies[i].gameObject->position.y);
					}
					else
					{
						ImGui::Text(" - gameObject net id: (null)");
					}
					
					ImGui::Separator();
				}
			}

			ImGui::Checkbox("Render colliders", &App->modRender->mustRenderColliders);
		}
	}
}

void ModuleNetworkingServer::onPacketReceived(const InputMemoryStream &packet, const sockaddr_in &fromAddress)
{
	if (state == ServerState::Listening)
	{
		uint32 protoId;
		packet >> protoId;
		if (protoId != PROTOCOL_ID) return;

		ClientMessage message;
		packet >> message;

		ClientProxy *proxy = getClientProxy(fromAddress);

		if (message == ClientMessage::Hello)
		{
			if (proxy == nullptr)
			{
				proxy = createClientProxy();

				if (proxy != nullptr)
				{
					std::string playerName;
					uint8 classType;
					packet >> playerName;
					packet >> classType;

					proxy->address.sin_family = fromAddress.sin_family;
					proxy->address.sin_addr.S_un.S_addr = fromAddress.sin_addr.S_un.S_addr;
					proxy->address.sin_port = fromAddress.sin_port;
					proxy->connected = true;
					proxy->name = playerName;
					proxy->clientId = nextClientId++;

					// Create new network object
					vec2 initialPosition = 1000.0f * vec2{ Random.next() - 0.5f, Random.next() - 0.5f};
					proxy->gameObject = spawnPlayer(classType, playerName, initialPosition, 0);
				}
				else
				{
					// NOTE(jesus): Server is full...
				}
			}

			if (proxy != nullptr)
			{
				// Send welcome to the new player
				OutputMemoryStream welcomePacket;
				welcomePacket << PROTOCOL_ID;
				welcomePacket << ServerMessage::Welcome;
				welcomePacket << proxy->clientId;
				welcomePacket << proxy->gameObject->networkId;
				sendPacket(welcomePacket, fromAddress);

				// Send all network objects to the new player
				uint16 networkGameObjectsCount;
				GameObject *networkGameObjects[MAX_NETWORK_OBJECTS];
				App->modLinkingContext->getNetworkGameObjects(networkGameObjects, &networkGameObjectsCount);
				for (uint16 i = 0; i < networkGameObjectsCount; ++i)
				{
					GameObject *gameObject = networkGameObjects[i];
					
					// TODO(you): World state replication lab session
					proxy->repManagerServer.create(gameObject->networkId);
				}

				LOG("Message received: hello - from player %s", proxy->name.c_str());
			}
			else
			{
				OutputMemoryStream unwelcomePacket;
				unwelcomePacket << PROTOCOL_ID;
				unwelcomePacket << ServerMessage::Unwelcome;
				sendPacket(unwelcomePacket, fromAddress);

				WLOG("Message received: UNWELCOMED hello - server is full");
			}
		}
		else if (message == ClientMessage::Input)
		{
			// Process the input packet and update the corresponding game object
			if (proxy != nullptr && IsValid(proxy->gameObject))
			{
				// TODO(you): Reliability on top of UDP lab session

				// Read input data
				while (packet.RemainingByteCount() > 0)
				{
					InputPacketData inputData;
					packet >> inputData.sequenceNumber;
					packet >> inputData.horizontalAxis;
					packet >> inputData.verticalAxis;
					packet >> inputData.buttonBits;
					packet >> inputData.mouseX;
					packet >> inputData.mouseY;
					packet >> inputData.mouseButtonBits;

					if (inputData.sequenceNumber >= proxy->nextExpectedInputSequenceNumber)
					{
						//Input
						proxy->gamepad.horizontalAxis = inputData.horizontalAxis;
						proxy->gamepad.verticalAxis = inputData.verticalAxis;
						unpackInputControllerButtons(inputData.buttonBits, proxy->gamepad);
						proxy->gameObject->behaviour->onInput(proxy->gamepad);

						//Mouse
						proxy->mouse.x = inputData.mouseX;
						proxy->mouse.y = inputData.mouseY;

						unpackMouseControllerButtons(inputData.mouseButtonBits, proxy->mouse);
						proxy->gameObject->behaviour->onMouseInput(proxy->mouse);

						proxy->nextExpectedInputSequenceNumber = inputData.sequenceNumber + 1;
					}
				}
			}
		} // TODO(you): UDP virtual connection lab session
		else if (message == ClientMessage::Ping) {
			if (proxy != nullptr)
			{
				proxy->secondsSinceLastReceivedPacket = 0.0f;
				proxy->deliveryManager.processAckdSequenceNumbers(packet);
			}
		}

		
	}
}

void ModuleNetworkingServer::onUpdate()
{
	if (state == ServerState::Listening)
	{
		// Handle networked game object destructions
		for (DelayedDestroyEntry &destroyEntry : netGameObjectsToDestroyWithDelay)
		{
			if (destroyEntry.object != nullptr)
			{
				destroyEntry.delaySeconds -= Time.deltaTime;
				if (destroyEntry.delaySeconds <= 0.0f)
				{
					destroyNetworkObject(destroyEntry.object);
					destroyEntry.object = nullptr;
				}
			}
		}

		secondsSinceSendPingPacket += Time.deltaTime;

		for (ClientProxy &clientProxy : clientProxies)
		{
			if (clientProxy.connected)
			{
				// TODO(you): UDP virtual connection lab session

				if (secondsSinceSendPingPacket >= PING_INTERVAL_SECONDS) {
					OutputMemoryStream pingPacket;
					pingPacket << PROTOCOL_ID;
					pingPacket << ServerMessage::Ping;
					sendPacket(pingPacket, clientProxy.address);
				}

				// Don't let the client proxy point to a destroyed game object
				if (!IsValid(clientProxy.gameObject))
				{
					clientProxy.gameObject = nullptr;
				}

				// TODO(you): World state replication lab session
				clientProxy.secondsSinceLastReplication += Time.deltaTime;
				if (clientProxy.secondsSinceLastReplication >= REPLICATION_INTERVAL_SECONDS) {
					OutputMemoryStream replicationPacket;
					replicationPacket << PROTOCOL_ID;
					replicationPacket.Write(ServerMessage::Replication);
					replicationPacket << clientProxy.nextExpectedInputSequenceNumber - 1;

					Delivery* delivery = clientProxy.deliveryManager.writeSequenceNumber(replicationPacket);
					if (delivery)
					{
						delivery->delegate = new ReplicationDeliveryDelegate(&clientProxy.repManagerServer);
					}

					clientProxy.repManagerServer.write(replicationPacket);
					sendPacket(replicationPacket, clientProxy.address);

					clientProxy.secondsSinceLastReplication = 0;
				}
				

				// TODO(you): Reliability on top of UDP lab session
				clientProxy.deliveryManager.processTimedOutPackets();


				clientProxy.secondsSinceLastReceivedPacket += Time.deltaTime;
				if (clientProxy.secondsSinceLastReceivedPacket >= DISCONNECT_TIMEOUT_SECONDS) {				
					destroyClientProxy(&clientProxy);
				}
			}
		}

		if (secondsSinceSendPingPacket >= PING_INTERVAL_SECONDS)
			secondsSinceSendPingPacket = 0;
	}
}

void ModuleNetworkingServer::onConnectionReset(const sockaddr_in & fromAddress)
{
	// Find the client proxy
	ClientProxy *proxy = getClientProxy(fromAddress);

	if (proxy)
	{
		// Clear the client proxy
		destroyClientProxy(proxy);
	}
}

void ModuleNetworkingServer::onDisconnect()
{
	uint16 netGameObjectsCount;
	GameObject* netGameObjects[MAX_NETWORK_OBJECTS] = {};
	App->modLinkingContext->getNetworkGameObjects(netGameObjects, &netGameObjectsCount);

	for (uint32 i = 0; i < netGameObjectsCount; ++i)
	{
		NetworkDestroy(netGameObjects[i]);
	}

	for (ClientProxy &clientProxy : clientProxies)
	{
		destroyClientProxy(&clientProxy);
	}
	
	for (DelayedDestroyEntry& destroyEntry : netGameObjectsToDestroyWithDelay)
	{
		destroyEntry.delaySeconds = 0.0f;
		destroyEntry.object = nullptr;
	}

	nextClientId = 0;
	secondsSinceSendPingPacket = 0;

	state = ServerState::Stopped;
}



//////////////////////////////////////////////////////////////////////
// Client proxies
//////////////////////////////////////////////////////////////////////

ModuleNetworkingServer::ClientProxy * ModuleNetworkingServer::createClientProxy()
{
	// If it does not exist, pick an empty entry
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (!clientProxies[i].connected)
		{
			return &clientProxies[i];
		}
	}

	return nullptr;
}

ModuleNetworkingServer::ClientProxy * ModuleNetworkingServer::getClientProxy(const sockaddr_in &clientAddress)
{
	// Try to find the client
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (clientProxies[i].address.sin_addr.S_un.S_addr == clientAddress.sin_addr.S_un.S_addr &&
			clientProxies[i].address.sin_port == clientAddress.sin_port)
		{
			return &clientProxies[i];
		}
	}

	return nullptr;
}

void ModuleNetworkingServer::destroyClientProxy(ClientProxy *clientProxy)
{
	// Destroy the object from all clients
	if (IsValid(clientProxy->gameObject))
	{
		std::list<GameObject*> relatedNetworkObjects;
		clientProxy->gameObject->behaviour->GetChildrenNetworkObjects(relatedNetworkObjects);

		for (GameObject* networkObject : relatedNetworkObjects)
		{
			destroyNetworkObject(networkObject);
		}
		destroyNetworkObject(clientProxy->gameObject);
	}
	clientProxy->deliveryManager.clear();
    *clientProxy = {};
}


//////////////////////////////////////////////////////////////////////
// Spawning
//////////////////////////////////////////////////////////////////////

GameObject * ModuleNetworkingServer::spawnPlayer(uint8 classType, std::string name, vec2 initialPosition, float initialAngle)
{
	// Create a new game object with the player properties
	GameObject *gameObject = NetworkInstantiate();
	gameObject->position = gameObject->initial_position = initialPosition;
	gameObject->size = { 65, 65 };
	gameObject->angle = initialAngle;

	// Create sprite
	gameObject->sprite = App->modRender->addSprite(gameObject);
	gameObject->sprite->order = 5;
	if (classType == 0) {
		gameObject->sprite->texture = App->modResources->berserkerIdle;

		gameObject->animation = App->modRender->addAnimation(gameObject);
		gameObject->animation->clip = App->modResources->playerIdleClip;
	}
	else if (classType == 1) {
		gameObject->sprite->texture = App->modResources->wizardIdle;

		gameObject->animation = App->modRender->addAnimation(gameObject);
		gameObject->animation->clip = App->modResources->playerIdleClip;
	}
	else {
		gameObject->sprite->texture = App->modResources->hunterIdle;

		gameObject->animation = App->modRender->addAnimation(gameObject);
		gameObject->animation->clip = App->modResources->playerIdleClip;
	}

	// Create collider
	gameObject->collider = App->modCollision->addCollider(ColliderType::Player, gameObject);
	gameObject->collider->isTrigger = true; // NOTE(jesus): This object will receive onCollisionTriggered events

	// Create behaviour
	Player* playerBehaviour = App->modBehaviour->addPlayer(gameObject);
	playerBehaviour->name = name;
	playerBehaviour->playerType = (PlayerType)classType;
	gameObject->behaviour = playerBehaviour;
	gameObject->behaviour->isServer = true;

	return gameObject;
}


//////////////////////////////////////////////////////////////////////
// Update / destruction
//////////////////////////////////////////////////////////////////////

GameObject * ModuleNetworkingServer::instantiateNetworkObject()
{
	// Create an object into the server
	GameObject * gameObject = Instantiate();

	// Register the object into the linking context
	App->modLinkingContext->registerNetworkGameObject(gameObject);

	// Notify all client proxies' replication manager to create the object remotely
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (clientProxies[i].connected)
		{
			// TODO(you): World state replication lab session
			clientProxies[i].repManagerServer.create(gameObject->networkId);
		}
	}

	return gameObject;
}

//Instantiates object to all clients excluding the one with passed network id
GameObject* ModuleNetworkingServer::instantiateNetworkObjectExcluding(uint32 playerNetworkId)
{
	// Create an object into the server
	GameObject* gameObject = Instantiate();

	// Register the object into the linking context
	App->modLinkingContext->registerNetworkGameObject(gameObject);

	// Notify all client proxies' replication manager to create the object remotely
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (clientProxies[i].connected && clientProxies[i].gameObject->networkId != playerNetworkId)
		{
			// TODO(you): World state replication lab session
			clientProxies[i].repManagerServer.create(gameObject->networkId);
		}
	}

	return gameObject;
}

void ModuleNetworkingServer::updateNetworkObject(GameObject * gameObject)
{
	// Notify all client proxies' replication manager to destroy the object remotely
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (clientProxies[i].connected)
		{
			// TODO(you): World state replication lab session
			clientProxies[i].repManagerServer.update(gameObject->networkId);
		}
	}
}

void ModuleNetworkingServer::destroyNetworkObject(GameObject * gameObject)
{
	// Notify all client proxies' replication manager to destroy the object remotely
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (clientProxies[i].connected)
		{
			// TODO(you): World state replication lab session
			clientProxies[i].repManagerServer.destroy(gameObject->networkId);
		}
	}

	// Assuming the message was received, unregister the network identity
	App->modLinkingContext->unregisterNetworkGameObject(gameObject);

	// Finally, destroy the object from the server
	Destroy(gameObject);
}

void ModuleNetworkingServer::destroyNetworkObject(GameObject * gameObject, float delaySeconds)
{
	uint32 emptyIndex = MAX_GAME_OBJECTS;
	for (uint32 i = 0; i < MAX_GAME_OBJECTS; ++i)
	{
		if (netGameObjectsToDestroyWithDelay[i].object == gameObject)
		{
			float currentDelaySeconds = netGameObjectsToDestroyWithDelay[i].delaySeconds;
			netGameObjectsToDestroyWithDelay[i].delaySeconds = min(currentDelaySeconds, delaySeconds);
			return;
		}
		else if (netGameObjectsToDestroyWithDelay[i].object == nullptr)
		{
			if (emptyIndex == MAX_GAME_OBJECTS)
			{
				emptyIndex = i;
			}
		}
	}

	ASSERT(emptyIndex < MAX_GAME_OBJECTS);

	netGameObjectsToDestroyWithDelay[emptyIndex].object = gameObject;
	netGameObjectsToDestroyWithDelay[emptyIndex].delaySeconds = delaySeconds;
}


//////////////////////////////////////////////////////////////////////
// Global create / update / destruction of network game objects
//////////////////////////////////////////////////////////////////////

GameObject * NetworkInstantiate()
{
	ASSERT(App->modNetServer->isConnected());

	return App->modNetServer->instantiateNetworkObject();
}

//Instantiates network object to all players except the passed one
GameObject* NetworkInstantiateExcluding(uint32 playerNetworkID)
{
	ASSERT(App->modNetServer->isConnected());

	return App->modNetServer->instantiateNetworkObjectExcluding(playerNetworkID);
}

void NetworkUpdate(GameObject * gameObject)
{
	ASSERT(App->modNetServer->isConnected());
	ASSERT(gameObject->networkId != 0);

	App->modNetServer->updateNetworkObject(gameObject);
}

void NetworkDestroy(GameObject * gameObject)
{
	NetworkDestroy(gameObject, 0.0f);
}

void NetworkDestroy(GameObject * gameObject, float delaySeconds)
{
	ASSERT(App->modNetServer->isConnected());
	ASSERT(gameObject->networkId != 0);

	gameObject->toBeDestroyed = true;
	App->modNetServer->destroyNetworkObject(gameObject, delaySeconds);
}

