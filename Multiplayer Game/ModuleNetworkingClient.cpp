#include "ModuleNetworkingClient.h"


//////////////////////////////////////////////////////////////////////
// ModuleNetworkingClient public methods
//////////////////////////////////////////////////////////////////////


void ModuleNetworkingClient::setServerAddress(const char * pServerAddress, uint16 pServerPort)
{
	serverAddressStr = pServerAddress;
	serverPort = pServerPort;
}

void ModuleNetworkingClient::setPlayerInfo(const char * pPlayerName, uint8 pPlayerType)
{
	playerName = pPlayerName;
	playerType = pPlayerType;
}



//////////////////////////////////////////////////////////////////////
// ModuleNetworking virtual methods
//////////////////////////////////////////////////////////////////////

void ModuleNetworkingClient::onStart()
{
	if (!createSocket()) return;

	if (!bindSocketToPort(0)) {
		disconnect();
		return;
	}

	// Create remote address
	serverAddress = {};
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_port = htons(serverPort);
	int res = inet_pton(AF_INET, serverAddressStr.c_str(), &serverAddress.sin_addr);
	if (res == SOCKET_ERROR) {
		reportError("ModuleNetworkingClient::startClient() - inet_pton");
		disconnect();
		return;
	}

	state = ClientState::Connecting;

	inputDataFront = 0;
	inputDataBack = 0;

	secondsSinceLastHello = 9999.0f;
	secondsSinceLastInputDelivery = 0.0f;
	secondsSinceLastPing = 0.0f;
	secondsSinceLastReceivedPacket = 0.0f;
}

void ModuleNetworkingClient::onGui()
{
	if (state == ClientState::Stopped) return;

	if (ImGui::CollapsingHeader("ModuleNetworkingClient", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (state == ClientState::Connecting)
		{
			ImGui::Text("Connecting to server...");
		}
		else if (state == ClientState::Connected)
		{
			ImGui::Text("Connected to server");

			ImGui::Separator();

			ImGui::Text("Player info:");
			ImGui::Text(" - Id: %u", playerId);
			ImGui::Text(" - Name: %s", playerName.c_str());

			ImGui::Separator();

			ImGui::Text("Player info:");
			ImGui::Text(" - Type: %u", playerType);
			ImGui::Text(" - Network id: %u", networkId);

			vec2 playerPosition = {};
			GameObject *playerGameObject = App->modLinkingContext->getNetworkGameObject(networkId);
			if (playerGameObject != nullptr) {
				playerPosition = playerGameObject->position;
			}
			ImGui::Text(" - Coordinates: (%f, %f)", playerPosition.x, playerPosition.y);

			ImGui::Separator();

			ImGui::Text("Connection checking info:");
			ImGui::Text(" - Ping interval (s): %f", PING_INTERVAL_SECONDS);
			ImGui::Text(" - Disconnection timeout (s): %f", DISCONNECT_TIMEOUT_SECONDS);

			ImGui::Separator();

			ImGui::Text("Input:");
			ImGui::InputFloat("Delivery interval (s)", &inputDeliveryIntervalSeconds, 0.01f, 0.1f, 4);
		}
	}
}

void ModuleNetworkingClient::onPacketReceived(const InputMemoryStream &packet, const sockaddr_in &fromAddress)
{
	// TODO(you): UDP virtual connection lab session
	secondsSinceLastReceivedPacket = 0;

	uint32 protoId;
	packet >> protoId;
	if (protoId != PROTOCOL_ID) return;

	ServerMessage message;
	packet >> message;

	if (state == ClientState::Connecting)
	{
		if (message == ServerMessage::Welcome)
		{
			packet >> playerId;
			packet >> networkId;

			LOG("ModuleNetworkingClient::onPacketReceived() - Welcome from server");
			state = ClientState::Connected;
		}
		else if (message == ServerMessage::Unwelcome)
		{
			WLOG("ModuleNetworkingClient::onPacketReceived() - Unwelcome from server :-(");
			disconnect();
		}
	}
	else if (state == ClientState::Connected)
	{
		// TODO(you): World state replication lab session
		if (message == ServerMessage::Replication)
		{
			// TODO(you): Reliability on top of UDP lab session
			packet.Read(inputDataFront);
			if (deliveryManager.processSequenceNumber(packet)) {

				repManagerClient.read(packet);

				GameObject* playerGameObject = App->modLinkingContext->getNetworkGameObject(networkId);
				if (playerGameObject == nullptr)
					return;

				InputController prevInput;
				MouseController prevMouse;
				prevInput = inputControllerFromInputPacketData(inputData[inputDataFront % ArrayCount(inputData)], prevInput);
				prevMouse = mouseControllerFromInputPacketData(inputData[inputDataFront % ArrayCount(inputData)], prevMouse);

				for (int i = inputDataFront + 1; i < inputDataBack; i++) {

					prevInput = inputControllerFromInputPacketData(inputData[i % ArrayCount(inputData)], prevInput);
					playerGameObject->behaviour->onInput(prevInput);
					prevMouse = mouseControllerFromInputPacketData(inputData[i % ArrayCount(inputData)], prevMouse);
					playerGameObject->behaviour->onMouseInput(prevMouse);
				}
			}
		}	
	}
}

void ModuleNetworkingClient::onUpdate()
{
	if (state == ClientState::Stopped) return;


	// TODO(you): UDP virtual connection lab session
	if (state == ClientState::Connecting)
	{
		secondsSinceLastHello += Time.deltaTime;

		if (secondsSinceLastHello > 0.1f)
		{
			secondsSinceLastHello = 0.0f;

			OutputMemoryStream packet;
			packet << PROTOCOL_ID;
			packet << ClientMessage::Hello;
			packet << playerName;
			packet << playerType;

			sendPacket(packet, serverAddress);
		}
	}
	else if (state == ClientState::Connected)
	{
		// TODO(you): UDP virtual connection lab session
		secondsSinceLastPing += Time.deltaTime;
		secondsSinceLastReceivedPacket += Time.deltaTime;

		if (secondsSinceLastReceivedPacket >= DISCONNECT_TIMEOUT_SECONDS) {
			disconnect();
			return;
		}

		if (secondsSinceLastPing >= PING_INTERVAL_SECONDS) {
			//Send ping packet
			OutputMemoryStream packet;
			packet << PROTOCOL_ID;
			packet << ClientMessage::Ping;
			deliveryManager.writeSequenceNumbersPendingAck(packet);

			sendPacket(packet, serverAddress);
			secondsSinceLastPing = 0;
		}

		// Process more inputs if there's space
		if (inputDataBack - inputDataFront < ArrayCount(inputData))
		{
			// Pack current input
			uint32 currentInputData = inputDataBack++;
			InputPacketData &inputPacketData = inputData[currentInputData % ArrayCount(inputData)];
			inputPacketData.sequenceNumber = currentInputData;
			inputPacketData.horizontalAxis = Input.horizontalAxis;
			inputPacketData.verticalAxis = Input.verticalAxis;
			inputPacketData.buttonBits = packInputControllerButtons(Input);

			//Pack current mouse input
			vec2 mousePosition = App->modRender->ScreenToWorld({ (float)Mouse.x, (float)Mouse.y });
			inputPacketData.mouseX = mousePosition.x;
			inputPacketData.mouseY = mousePosition.y;
			inputPacketData.mouseButtonBits = packMouseControllerButtons(Mouse);


			// TODO(you): Latency management lab session
			GameObject* playerGameObject = App->modLinkingContext->getNetworkGameObject(networkId);
			if (playerGameObject != nullptr) {
				playerGameObject->behaviour->onInput(Input);
				Mouse.x = mousePosition.x;
				Mouse.y = mousePosition.y;
				playerGameObject->behaviour->onMouseInput(Mouse);
			}
		}
		secondsSinceLastInputDelivery += Time.deltaTime;

		// Input delivery interval timed out: create a new input packet
		if (secondsSinceLastInputDelivery > inputDeliveryIntervalSeconds)
		{
			secondsSinceLastInputDelivery = 0.0f;

			OutputMemoryStream packet;
			packet << PROTOCOL_ID;
			packet << ClientMessage::Input;

			// TODO(you): Reliability on top of UDP lab session

			for (uint32 i = inputDataFront; i < inputDataBack; ++i)
			{
				InputPacketData &inputPacketData = inputData[i % ArrayCount(inputData)];
				packet << inputPacketData.sequenceNumber;
				packet << inputPacketData.horizontalAxis;
				packet << inputPacketData.verticalAxis;
				packet << inputPacketData.buttonBits;
				packet << inputPacketData.mouseX;
				packet << inputPacketData.mouseY;
				packet << inputPacketData.mouseButtonBits;
			}

			// Clear the queue
			//inputDataFront = inputDataBack;

			sendPacket(packet, serverAddress);
		}

		// Update camera for player
		GameObject *playerGameObject = App->modLinkingContext->getNetworkGameObject(networkId);
		if (playerGameObject != nullptr)
		{
			App->modRender->cameraPosition = playerGameObject->position;
		}
		else
		{
			// This means that the player has been destroyed (e.g. killed)
		}

		// Interpolation of other objects
		uint16 networkObjectsCount = 0;
		GameObject* networkGameObjects[MAX_NETWORK_OBJECTS] = {};
		App->modLinkingContext->getNetworkGameObjects(networkGameObjects, &networkObjectsCount);

		for (int i = 0; i < networkObjectsCount; ++i)
		{
			if (networkGameObjects[i]->networkId != networkId)
				networkGameObjects[i]->Interpolate();
		}
	}
}

void ModuleNetworkingClient::onConnectionReset(const sockaddr_in & fromAddress)
{
	disconnect();
}

void ModuleNetworkingClient::onDisconnect()
{
	state = ClientState::Stopped;

	GameObject *networkGameObjects[MAX_NETWORK_OBJECTS] = {};
	uint16 networkGameObjectsCount;
	App->modLinkingContext->getNetworkGameObjects(networkGameObjects, &networkGameObjectsCount);
	App->modLinkingContext->clear();

	for (uint32 i = 0; i < networkGameObjectsCount; ++i)
	{
		Destroy(networkGameObjects[i]);
	}

	deliveryManager.clear();
	App->modRender->cameraPosition = {};
}
