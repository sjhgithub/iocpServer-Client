﻿#include "iocp.h"

bool IOCP_Server::initServer()
{
	WSADATA wsaData;

	int nRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (0 != nRet) {
		spdlog::error("WSAStartup() Function failure : {}", WSAGetLastError());
		return false;
	}

	//연결지향형 TCP , Overlapped I/O 소켓을 생성
	listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSA_FLAG_OVERLAPPED);

	if (INVALID_SOCKET == listenSocket) {
		spdlog::error("socket() Function failure : {}", WSAGetLastError());
		return false;
	}

	Timer_Event* t = new Timer_Event;
	t->object_id = 0;
	t->exec_time = high_resolution_clock::now() + 5120ms;
	t->event = T_DisconnectRemove;
	timer.setTimerEvent(*t);

	spdlog::info("Socket Init Success..!");
	return true;
}

bool IOCP_Server::BindandListen(const u_short port)
{
	SOCKADDR_IN		stServerAddr;
	stServerAddr.sin_family = AF_INET;
	stServerAddr.sin_port = htons(port); //서버 포트를 설정한다.		
	// 어떤 주소에서 들어오는 접속이라도 받아들이겠다.
	// 보통 서버라면 이렇게 설정한다. 만약 한 아이피에서만 접속을 받고 싶다면
	// 그 주소를 inet_addr함수를 이용해 넣으면 된다.
	stServerAddr.sin_addr.s_addr = INADDR_ANY;

	// 위에서 지정한 서버 주소 정보와 cIOCompletionPort 소켓을 연결한다.
	//int nRet = ::bind(listenSocket, (SOCKADDR*)&stServerAddr, sizeof(SOCKADDR_IN));
	int nRet = ::bind(listenSocket, reinterpret_cast<sockaddr *>(&stServerAddr), sizeof(SOCKADDR_IN));
	if (0 != nRet) {
		spdlog::error("bind() Function failure : {}", WSAGetLastError());
		return false;
	}

	// 접속 요청을 받아들이기 위해 cIOCompletionPort소켓을 등록하고 
	// 접속 대기큐를 5개로 설정 한다.
	nRet = ::listen(listenSocket, 5);
	if (0 != nRet) {
		spdlog::error("listen() Function failure : {}", WSAGetLastError());
		return false;
	}

	// 네이글 알고리즘 OFF
	int option = TRUE;
	setsockopt(listenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&option, sizeof(option));


	spdlog::info("Server Registration Successful..!");
	return true;
}

bool IOCP_Server::StartServer()
{
	// CompletionPort객체 생성 요청을 한다.
	g_hiocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, MAX_WORKERTHREAD);
	if (NULL == g_hiocp) {
		spdlog::error("CreateIoCompletionPort() Function failure : {}", GetLastError());
		return false;
	}

	// 접속된 클라이언트 주소 정보를 저장할 구조체
	bool bRet = CreateWorkerThread();
	if (false == bRet) {
		return false;
	}

	bRet = CreateAccepterThread();
	if (false == bRet) {
		return false;
	}

	spdlog::info("IOCP Server Start..!");
	return true;
}

void IOCP_Server::initClient()
{
	// 미리 player, player_session 공간을 할당 한다.
	player_session.reserve(CS.get_max_player());
	player.reserve(CS.get_max_player());
}

void IOCP_Server::destroyThread()
{
	// Worker Thread 종료한다.
	mIsWorkerRun = false;
	CloseHandle(g_hiocp);
	for (auto& th : mIOWorkerThreads) {
		if (th.joinable()) {
			th.join();
		}
	}

	// Accepter Thread 종료한다.
	mIsAccepterRun = false;
	closesocket(listenSocket);

	if (mAccepterThread.joinable()) {
		mAccepterThread.join();
	}

	// Timer, API Thread를 종료 한다.
	api.stop();
	timer.stop();
}

void IOCP_Server::add_tempUniqueNo(unsigned_int64 uniqueNo)
{
	tempUniqueNo.push(uniqueNo);
}

bool IOCP_Server::CreateWorkerThread()
{
	// Vector 공간 미리 할당
	mIOWorkerThreads.reserve(MAX_WORKERTHREAD + 1);

	// WaingThread Queue에 대기 상태로 넣을 쓰레드들 생성 권장되는 개수 : (cpu개수 * 2) + 1 
	for (int i = 0; i < MAX_WORKERTHREAD; i++) {
		mIOWorkerThreads.emplace_back([this]() { WorkerThread(); });
	}

	spdlog::info("WorkerThread Start..!");
	return true;
}

void IOCP_Server::WorkerThread()
{
	//CompletionKey를 받을 포인터 변수
	PLAYER_Session* pPlayerSession = NULL;
	//함수 호출 성공 여부
	BOOL bSuccess = TRUE;
	//Overlapped I/O작업에서 전송된 데이터 크기
	DWORD dwIoSize = 0;
	//I/O 작업을 위해 요청한 Overlapped 구조체를 받을 포인터
	LPOVERLAPPED lpOverlapped = NULL;

	while (mIsWorkerRun)
	{
		//////////////////////////////////////////////////////
		// 이 함수로 인해 쓰레드들은 WaitingThread Queue에
		// 대기 상태로 들어가게 된다.
		// 완료된 Overlapped I/O작업이 발생하면 IOCP Queue에서
		// 완료된 작업을 가져와 뒤 처리를 한다.
		// 그리고 PostQueuedCompletionStatus()함수에의해 사용자
		// 메세지가 도착되면 쓰레드를 종료한다.
		//////////////////////////////////////////////////////
		bSuccess = GetQueuedCompletionStatus(g_hiocp,
			&dwIoSize,					// 실제로 전송된 바이트
			(PULONG_PTR)&pPlayerSession,	// CompletionKey
			&lpOverlapped,				// Overlapped IO 객체
			INFINITE);					// 대기할 시간

		// 사용자 쓰레드 종료 메세지 처리..
		if (TRUE == bSuccess && 0 == dwIoSize && NULL == lpOverlapped)
		{
			mIsWorkerRun = false;
			continue;
		}

		if (NULL == lpOverlapped)
		{
			continue;
		}

		// client가 접속을 끊었을때..			
		if (FALSE == bSuccess || (0 == dwIoSize && TRUE == bSuccess))
		{
			spdlog::info("[Disconnect] SOCKET : {} || [unique_no:{}]", (int)pPlayerSession->get_sock(), (int)pPlayerSession->get_unique_no());
			ClosePlayer(pPlayerSession->get_unique_no());
			CloseSocket(pPlayerSession);
			continue;
		}

		stOverlappedEx* pOverlappedEx = (stOverlappedEx*)lpOverlapped;
		// Overlapped I/O Recv작업 결과 뒤 처리
		switch (pOverlappedEx->m_eOperation) {

		case IOOperation::RECV:
		{
			OnRecv(pOverlappedEx, dwIoSize);
		}
		break;

		case IOOperation::SEND:
		{
			// Overlapped I/O Send작업 결과 뒤 처리
			OnSend(pPlayerSession, dwIoSize);
			//spdlog::info("[SEND] bytes : {} , msg : {}", dwIoSize, pOverlappedEx->m_wsaBuf.buf);
		}
		break;

		case IOOperation::DisconnectRemove:
		{
			// 종료된 리스트를 비워준다.
			disconnectUniqueNo.clear();
			Timer_Event* t = new Timer_Event;
			t->object_id = 0;
			t->exec_time = high_resolution_clock::now() + 5120ms;
			t->event = T_DisconnectRemove;
			timer.setTimerEvent(*t);
		}
		break;

		default:
		{
			// 예외 상황
			spdlog::critical("[Exception WorkerThread({})] No value defined..! || [unique_no:{}]",
				(int)pOverlappedEx->m_eOperation, pPlayerSession->get_unique_no());
		}
		break;

		}
	}
}

void IOCP_Server::CloseSocket(class PLAYER_Session * pPlayerSession, bool bIsForce)
{
	struct linger stLinger = { 0, 0 };	// SO_DONTLINGER로 설정

	// bIsForce가 true이면 SO_LINGER, timeout = 0으로 설정하여 강제 종료 시킨다. 주의 : 데이터 손실이 있을수 있음 
	if (true == bIsForce)
	{
		stLinger.l_onoff = 1;
	}

	//socketClose소켓의 데이터 송수신을 모두 중단 시킨다.
	shutdown(pPlayerSession->get_sock(), SD_BOTH);

	//소켓 옵션을 설정한다.
	setsockopt(pPlayerSession->get_sock(), SOL_SOCKET, SO_LINGER, (char*)&stLinger, sizeof(stLinger));

	//소켓 연결을 종료 시킨다. 
	closesocket(pPlayerSession->get_sock());
	pPlayerSession->get_sock() = INVALID_SOCKET;
}

void IOCP_Server::ClosePlayer(unsigned_int64 uniqueNo)
{
	player.erase(uniqueNo);
	player_session.erase(uniqueNo);
}

bool IOCP_Server::SendPacket(unsigned_int64 uniqueNo, char * pMsg, int nLen)
{
	auto pPlayerSession = getSessionByNo(uniqueNo);
	if (pPlayerSession != nullptr) {
		// send_Buffer에 pMsg를 넣어준다.
		pPlayerSession->sendReady(pMsg, nLen);
		// Send처리를 한다.
		return pPlayerSession->sendIo();
	}
	else {
		return false;
	}
}

PLAYER_Session * IOCP_Server::getSessionByNo(unsigned_int64 uniqueNo)
{
	auto pTempPlayerSession = player_session.find(uniqueNo);
	if (pTempPlayerSession == player_session.end()) {
		if (disconnectUniqueNo.find(uniqueNo) == disconnectUniqueNo.end()) {
			spdlog::error("[getSessionByNo] No Exit Session || [unique_no:{}]", uniqueNo);
			disconnectUniqueNo.insert(pair<unsigned_int64, bool>(uniqueNo, true));
		}
		return nullptr;
	}
	auto pPlayerSession = pTempPlayerSession->second;

	return pPlayerSession;
}

void IOCP_Server::OnRecv(struct stOverlappedEx* pOver, int ioSize)
{
	// 플레이어 세션 에서 플레이어 데이터 가져오기
	auto pPlayerSession = getSessionByNo(pOver->m_unique_no);

	//std::cout << "User No : " << pPlayerSession->get_unique_no() << " | OnRecv..!" << std::endl;
	//std::cout << "[INFO] ioSize : " << ioSize << std::endl;

	// 쓰기를 위한 위치를 옮겨준다.
	if (!pPlayerSession->read_buffer().moveWritePos(ioSize))
	{
		spdlog::error("ReadBuffer Over Flow || [unique_no:{}]", pPlayerSession->get_unique_no());
	}

	PACKET_HEADER header;
	//int remainSize = 0;
	pPlayerSession->set_remainSize(0);
	while (pPlayerSession->read_buffer().getReadAbleSize() > 0) {

		// 일정 개수 이상 Packet 오류가 나면 강제 종료 시킨다.
		if (pPlayerSession->get_error_cnt() >= CS.get_limit_err_cnt()) {
			spdlog::error("Limit Error Count Maximum Exceeded / ErrorCnt({}) >= LimitCnt({}) || [unique_no:{}]",
				pPlayerSession->get_error_cnt(), CS.get_limit_err_cnt(), pPlayerSession->get_unique_no());
			ClosePlayer(pPlayerSession->get_unique_no());
			CloseSocket(pPlayerSession);
			break;
		}

		// 읽을 수 있는 Packet 크기가 Header Packet 보다 작을 경우 처리 한다.
		if (pPlayerSession->read_buffer().getReadAbleSize() <= sizeof(header)) {
			break;
		}

		// Packet_Header 를 가져온다.
		auto PacketSize = pPlayerSession->read_buffer().getHeaderSize((char*)&header, sizeof(header));
		if (PacketSize == -1) {
			spdlog::error("getHeaderSize || [unique_no:{}]", pPlayerSession->get_unique_no());
		}

		if (pPlayerSession->read_buffer().getReadAbleSize() < header.packet_len || header.packet_len <= PACKET_HEADER_BYTE) {
			// 읽을 수 있는 Packet 사이즈가 전송된 Packet의 전체 사이즈보다 작을 경우 처리를 한다.
			spdlog::critical("Packet Header Critical AbleSize({}) <= PacketSize({}) OR getReadAbleSize({}) < PacketSize({}) || [unique_no:{}]",
				header.packet_len, PACKET_HEADER_BYTE, pPlayerSession->read_buffer().getReadAbleSize(), header.packet_len, pPlayerSession->get_unique_no());
			// Packet 사이즈가 Header 크기보다 작을 경우 Error count를 올린다.
			if (header.packet_len <= PACKET_HEADER_BYTE) {
				pPlayerSession->update_error_cnt();
			}
			break;
		}
		else {
			// 실제로 처리를 하는 위치

			// API 라이브러리로 해당 값을 전달 시켜 준다.
			ProtocolType protocolBase = (ProtocolType)((int)header.packet_type / (int)PACKET_RANG_SIZE * (int)PACKET_RANG_SIZE);

			api.packet_Add(pPlayerSession->get_unique_no(), pPlayerSession->read_buffer().getReadBuffer(), header.packet_len);

			// 읽기 완료 처리
			pPlayerSession->read_buffer().moveReadPos(header.packet_len);
			pPlayerSession->incr_remainSize((ioSize - header.packet_len));
			//remainSize += (ioSize - header.packet_len);
		}

	}

	// 임시 번호의 경우 일시적으로 BindRecv를 하지 않는다.
	// L_Auth_Login에서 처리하면서 BindRecv를 해준다.
	if (pPlayerSession->get_unique_no() >= UNIQUE_START_NO) {
		BindRecv(pPlayerSession, pPlayerSession->get_remainSize());
	}
}

void IOCP_Server::OnSend(PLAYER_Session * pSession, int size)
{
	// send 완료 처리
	pSession->sendFinish(size);
}

bool IOCP_Server::CreateAccepterThread()
{
	mAccepterThread = std::thread([this]() { AccepterThread(); });

	spdlog::info("AccepterThread Start..!");
	return true;
}

void IOCP_Server::AccepterThread()
{
	SOCKADDR_IN client_addr;
	auto client_len = static_cast<int>(sizeof(client_addr));


	while (mIsAccepterRun)
	{
		// 접속 받을 유저 소켓을 생성 한다.
		PLAYER_Session* pPlayerSession = new PLAYER_Session;
		pPlayerSession->set_init_session();


		// 클라이언트 접속 요청이 들어올 때까지 기다린다.
		pPlayerSession->get_sock() = WSAAccept(listenSocket, reinterpret_cast<sockaddr *>(&client_addr), &client_len, NULL, NULL);
		if (INVALID_SOCKET == pPlayerSession->get_sock()) {
			continue;
		}
		else if (player_session.size() >= CS.get_max_player()) {
			spdlog::critical("Client Full..! sessionSize({}) >= MAX_PLAYER({})", player_session.size(), CS.get_max_player());
			CloseSocket(pPlayerSession);
			continue;
		}

		// I/O Completion Port객체와 소켓을 연결시킨다.
		bool bRet = BindIOCompletionPort(pPlayerSession);
		if (false == bRet) {
			spdlog::error("BindIOCompletionPort() Function failure : {}", GetLastError());
			CloseSocket(pPlayerSession);
			continue;
		}

		// 임시 uniqueNo가 없을 경우 강제로 연결을 끊는다.
		if (tempUniqueNo.size() == 0) {
			spdlog::critical("tempUniqueNo Full..!");
			CloseSocket(pPlayerSession);
			continue;
		}

		// session에 set 해준다.
		pPlayerSession->set_unique_no(tempUniqueNo.front());

		// player_session에 추가 한다.
		player_session.insert(std::unordered_map<unsigned_int64, class PLAYER_Session *>::value_type(tempUniqueNo.front(), pPlayerSession));

		// 플레이어를 set 해준다.
		class PLAYER * acceptPlayer = new class PLAYER;
		acceptPlayer->set_sock(pPlayerSession->get_sock());
		acceptPlayer->set_unique_no(tempUniqueNo.front());
		player.insert(std::unordered_map<unsigned_int64, class PLAYER *>::value_type(tempUniqueNo.front(), acceptPlayer));

		//클라이언트 갯수 증가
		tempUniqueNo.pop();

		char clientIP[32] = { 0, };
		inet_ntop(AF_INET, &(client_addr.sin_addr), clientIP, 32 - 1);
		spdlog::info("[Connect] Client IP : {} / SOCKET : {} || [unique_no:{}]", clientIP, (int)pPlayerSession->get_sock(), pPlayerSession->get_unique_no());

		// Recv Overlapped I/O작업을 요청해 놓는다.
		bRet = BindRecv(pPlayerSession, 0);
		if (false == bRet) {
			spdlog::error("BindRecv() Function failure || [unique_no:{}]", pPlayerSession->get_unique_no());
			ClosePlayer(pPlayerSession->get_unique_no());
			CloseSocket(pPlayerSession);
			continue;
		}

	}
}

bool IOCP_Server::BindIOCompletionPort(class PLAYER_Session * pPlayerSession)
{
	// socket과 pPlayerSession를 CompletionPort객체와 연결시킨다.
	auto hIOCP = CreateIoCompletionPort((HANDLE)pPlayerSession->get_sock(), g_hiocp, (ULONG_PTR)(pPlayerSession), 0);

	if (NULL == hIOCP || g_hiocp != hIOCP) {
		spdlog::error("CreateIoCompletionPort() Function failure : {} || [unique_no:{}]", GetLastError(), pPlayerSession->get_unique_no());
		return false;
	}

	return true;
}

bool IOCP_Server::BindRecv(class PLAYER_Session * pPlayerSession, int remainSize)
{
	DWORD dwFlag = 0;
	DWORD dwRecvNumBytes = 0;
	WSABUF wBuf;

	if (remainSize > 0) {
		pPlayerSession->read_buffer().checkWrite(remainSize);
	}

	// Overlapped I/O을 위해 각 정보를 셋팅해 준다.
	wBuf.len = MAX_SOCKBUF;
	wBuf.buf = pPlayerSession->read_buffer().getWriteBuffer();
	pPlayerSession->get_Recv_over().m_eOperation = IOOperation::RECV;
	pPlayerSession->get_Recv_over().m_unique_no = pPlayerSession->get_unique_no();
	pPlayerSession->get_Recv_over().m_socketSession = pPlayerSession->get_sock();

	int nRet = WSARecv(pPlayerSession->get_sock(),
		&wBuf,
		1,
		&dwRecvNumBytes,
		&dwFlag,
		(LPWSAOVERLAPPED) & (pPlayerSession->get_Recv_over()),
		NULL);

	// socket_error이면 client socket이 끊어진걸로 처리한다.
	if (nRet == SOCKET_ERROR && (WSAGetLastError() != ERROR_IO_PENDING)) {
		spdlog::error("WSARecv() Function failure : {} || [unique_no:{}]", WSAGetLastError(), pPlayerSession->get_unique_no());
		ClosePlayer(pPlayerSession->get_unique_no());
		CloseSocket(pPlayerSession);
		return false;
	}

	return true;
}

IOCP_Server::IOCP_Server()
{
	std::wcout.imbue(std::locale("korean"));	// Locale Korean
	g_hiocp = INVALID_HANDLE_VALUE;
	listenSocket = INVALID_SOCKET;
	uniqueNo = UNIQUE_START_NO;
	// 임시 uniqueNo 추가
	for (int i = 0; i < UNIQUE_START_NO; ++i) {
		tempUniqueNo.push(i);
	}
	mIsWorkerRun = true;
	mIsAccepterRun = true;
}

IOCP_Server::~IOCP_Server()
{
	WSACleanup();
	destroyThread();
}
