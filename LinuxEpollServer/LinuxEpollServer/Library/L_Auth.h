﻿#ifndef __L_AUTH_H__
#define __L_AUTH_H__

#include "Api.h"

class AuthRoute {
public:
	AuthRoute();
	~AuthRoute();
	void ApiProcessing(Packet_Frame packet, sc_packet_result& resultCode);	// API 처리

private:
	class AuthModule *auth;
};


#endif