﻿#include "Main.h"

class IOCP_Client iocp_client;
class Logic_API api;
class PLAYER *Player;

int main() {
	bool result;
	result = iocp_client.initClient();
	if (result == false) {
		exit(1);
	}
	result = iocp_client.connectServer(ipAddres);
	if (result == false) {
		exit(1);
	}
	api.start();

	// 로그인
	cs_packet_auth sendPacket;
	sendPacket.packet_len = sizeof(sendPacket);
	sendPacket.packet_type = CLIENT_AUTH_LOGIN;
	strcpy(sendPacket.sha256sum, "0998deee89b70c6b4e68a15a731bfc86bb1707d32d9825035819d8a338172bcf");
	iocp_client.SendPacket(reinterpret_cast<char *>(&sendPacket), sizeof(sendPacket));

	Sleep(10);

	cs_packet_dir sendPacket2;
	sendPacket2.packet_len = sizeof(sendPacket2);
	sendPacket2.packet_type = CLIENT_AUTH_TEST;
	sendPacket2.dir.x = 06;
	sendPacket2.dir.y = 16;
	iocp_client.SendPacket(reinterpret_cast<char *>(&sendPacket2), sizeof(sendPacket2));

	//for (int i = 0; i < 5; ++i) {
	//	spdlog::info("{}", sizeof(sendPacket));
	//	iocp_client.SendPacket(reinterpret_cast<char *>(&sendPacket), sizeof(sendPacket));
	//}

	//Sleep(10);

	//sendPacket.packet_len = sizeof(sendPacket);
	//sendPacket.packet_type = CLIENT_AUTH_LOGIN;
	//strcpy(sendPacket.sha256sum, "0998deee89b70c6b4e68a15a731bfc86bb1707d32d9825035819d8a338172bcf");
	//for (int i = 0; i < 10; ++i) {
	//	spdlog::info("{}", sizeof(sendPacket));
	//	iocp_client.SendPacket(reinterpret_cast<char *>(&sendPacket), sizeof(sendPacket));
	//}

	
	getchar();
}
