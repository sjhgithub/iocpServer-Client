#include "EpollServer.h"

Epoll_Server::Epoll_Server()
{
	memset(&ev, 0, sizeof ev);
	mIsEventThreadRun = false;
	mIsWorkerThreadRun = false;
	disconnectUniqueNo.clear();
	// 임시 uniqueNo 추가
	for (int i = 0; i < UNIQUE_START_NO; ++i) {
		tempUniqueNo.push(i);
	}
}

Epoll_Server::~Epoll_Server()
{
}

void Epoll_Server::init_server()
{
	if ((epfd = epoll_create(MAX_EVENTS)) < 0) {
		spdlog::error("epoll_create() Function failure");
	}

	if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		spdlog::error("socket() Function failure");
		exit(EXIT_FAILURE);
	}

}

void Epoll_Server::BindandListen(int port)
{
	memset(&sin, 0, sizeof sin);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *) &sin, sizeof sin) < 0) {
		close(sock);
		spdlog::error("bind() Function failure");
		exit(EXIT_FAILURE);
	}

	if (listen(sock, BACKLOG) < 0) {
		close(sock);
		spdlog::error("listen() Function failure");
		exit(EXIT_FAILURE);
	}

	mIsEventThreadRun = true;
	mEventThread = std::thread([this]() { EventThread(); });

	mIsWorkerThreadRun = true;
	mWorkerThreads.reserve(MAX_WORKERTHREAD + 1);
	for (int i = 0; i < 1; i++) {
		mWorkerThreads.emplace_back([this]() { WorkerThread(); });
	}

	spdlog::info("Epoll Server Thread Start..!");
	ev.events = EPOLLIN;
	ev.data.fd = sock;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
}

void Epoll_Server::add_tempUniqueNo(unsigned_int64 uniqueNo)
{
	tempUniqueNo.push(uniqueNo);
}

bool Epoll_Server::SendPacket(int sock, char* pMsg, int nLen)
{
	auto pPlayerSession = getSessionByNo(sock);
	if (pPlayerSession != nullptr) {
		// send_Buffer에 pMsg를 넣어준다.
		pPlayerSession->sendReady(pMsg, nLen);
		// Send처리를 한다.
		return pPlayerSession->sendIo();
	}
	else {
		return false;
	}
	return false;
}

PLAYER_Session * Epoll_Server::getSessionByNo(int sock)
{
	auto pTempPlayerSession = player_session.find(sock);
	if (pTempPlayerSession == player_session.end()) {
		if (disconnectUniqueNo.find(sock) == disconnectUniqueNo.end()) {
			spdlog::error("[getSessionByNo] No Exit Session || [socketNo:{}]", sock);
			disconnectUniqueNo.insert(pair<int, bool>(sock, true));
		}
		return nullptr;
	}
	auto pPlayerSession = pTempPlayerSession->second;

	return pPlayerSession;
}

void Epoll_Server::SetNonBlocking(int sock)
{
	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag | O_NONBLOCK);

	memset(&ev, 0, sizeof ev);
	ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
	ev.data.fd = sock;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
}

void Epoll_Server::EventThread()
{
	int nfds;
	while (mIsEventThreadRun)
	{
		// -1 : Event가 일어나기 전까지 계속 대기 한다.
		// 0  : Event가 일어나는지 상관 없이 조사만 하고 리턴
		// 1~ : 해당 시간동안 Event가 있는지 대기한다.
		nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

		if (nfds < 0) {
			// critical error
			//spdlog::error("epoll_wait() Function failure : {}", strerror(errno));
			continue;
			//exit(-1);
		}
		else if (nfds == 0) {
			// Event가 없다.
			continue;
		}

		for (int i = 0; i < nfds; i++) {
			if (events[i].data.fd == sock) {
				// 신규 유저 접속 처리
				AcceptProcessing(events[i]);
			}
			else {
				std::lock_guard<std::mutex> guard(mLock);
				// 해당 이벤트를 event_Queue에 넣어준다.
				struct epoll_event eev;
				memcpy(&eev, &events[i], sizeof(events[i]));
				event_Queue.push(eev);
			}
			
		}

	}
}

void Epoll_Server::WorkerThread()
{
	while (mIsWorkerThreadRun)
	{
		std::lock_guard<std::mutex> guard(mLock);
		if (!event_Queue.empty()) {
			auto event = event_Queue.front();
			event_Queue.pop();
			// 기존 접속자
			auto pPlayerSession = getSessionByNo(event.data.fd);
			if (pPlayerSession == nullptr) continue;
			if (event.events & EPOLLIN) {
				int ioSize = 0;
				errno = 0;	// Errno Clear
				if (pPlayerSession->get_remainSize() > 0) {
					pPlayerSession->read_buffer().checkWrite(pPlayerSession->get_remainSize());
				}

				/*spdlog::info("readPos : {}, writePos : {}, ReadAbleSize : {}",
					pPlayerSession->read_buffer().getReadPos(), pPlayerSession->read_buffer().getWritePos(),
					pPlayerSession->read_buffer().getReadAbleSize());*/

				ioSize = read(pPlayerSession->get_sock(), pPlayerSession->read_buffer().getWriteBuffer(), pPlayerSession->read_buffer().getWriteAbleSize());
				if (errno == CONNECTION_RESET) {
					spdlog::info("[Disconnect] EPOLLIN SOCKET : {}, errno : {} || [unique_no:{}]", (int)pPlayerSession->get_sock(), errno, (int)pPlayerSession->get_unique_no());
					ClosePlayer(pPlayerSession->get_sock(), &event);
					continue;
				}else if (ioSize == 0) {
					spdlog::info("[Disconnect] EPOLLIN SOCKET : {}, ioSize : {} || [unique_no:{}]", (int)pPlayerSession->get_sock(), ioSize, (int)pPlayerSession->get_unique_no());
					ClosePlayer(pPlayerSession->get_sock(), &event);
				}
				else if (ioSize < 0) {
					// Read Error
					if (errno != EAGAIN) {
						// Try again
						spdlog::error("[Exception WorkerThread()] Read Error ioSize : {}, Error : {}, events : {} || [unique_no:{}]",
							ioSize, errno, event.events, pPlayerSession->get_unique_no());
					}
				}
				else {
					// Recv 처리
					//spdlog::info("ioSize : {}", ioSize);
					OnRecv(event.data.fd, ioSize);
				}
			}
			else if (event.events & EPOLLERR) {
				spdlog::info("[Disconnect] EPOLLERR SOCKET : {} || [unique_no:{}]", (int)pPlayerSession->get_sock(), (int)pPlayerSession->get_unique_no());
				ClosePlayer(pPlayerSession->get_sock(), &event);
			}
			else  if (event.events & EPOLLOUT) {
				// sendIO
			}
			else if (event.events & EPOLLRDHUP) {
				spdlog::info("[Disconnect] EPOLLRDHUP SOCKET : {} || [unique_no:{}]", (int)pPlayerSession->get_sock(), (int)pPlayerSession->get_unique_no());
				ClosePlayer(pPlayerSession->get_sock(), &event);
			}else {
				spdlog::error("[Exception WorkerThread()] No Event ({}), Error : {} || [unique_no:{}]",
					ev.events, errno, pPlayerSession->get_unique_no());
			}
		}
		else {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
}

void Epoll_Server::ClosePlayer(const int sock, struct epoll_event *ev)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, sock, ev);
	player_session.erase(sock);
	player.erase(sock);
	close(sock);
}

bool Epoll_Server::AcceptProcessing(struct epoll_event &ev)
{
	// 신규 유저 접속 처리
	PLAYER_Session* pPlayerSession = new PLAYER_Session;
	pPlayerSession->set_init_session();

	struct sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof client_addr;

	pPlayerSession->get_sock() = accept(sock, (struct sockaddr *) &client_addr, &client_addr_len);
	if (pPlayerSession->get_sock() < 0) {
		// 정상적인 유저가 아니다.
		return false;
	}
	else if (player_session.size() >= CS.get_max_player()) {
		spdlog::critical("Client Full..! sessionSize({}) >= MAX_PLAYER({})", player_session.size(), CS.get_max_player());
		ClosePlayer(pPlayerSession->get_sock(), &ev);
		return false;
	}

	// 임시 uniqueNo가 없을 경우 강제로 연결을 끊는다.
	if (tempUniqueNo.size() == 0) {
		spdlog::critical("tempUniqueNo Full..!");
		ClosePlayer(pPlayerSession->get_sock(), &ev);
		return false;
	}

	// session에 set 해준다.
	pPlayerSession->set_unique_no(tempUniqueNo.front());

	// player_session에 추가 한다.
	player_session.insert(std::unordered_map<int, class PLAYER_Session *>::value_type(pPlayerSession->get_sock(), pPlayerSession));

	// 플레이어를 set 해준다.
	class PLAYER * acceptPlayer = new class PLAYER;
	acceptPlayer->set_sock(pPlayerSession->get_sock());
	acceptPlayer->set_unique_no(tempUniqueNo.front());
	player.insert(std::unordered_map<int, class PLAYER *>::value_type(pPlayerSession->get_sock(), acceptPlayer));

	//클라이언트 갯수 증가
	tempUniqueNo.pop();

	// fd에 통신 준비 처리
	SetNonBlocking(pPlayerSession->get_sock());

	char clientIP[32] = { 0, };
	inet_ntop(AF_INET, &(client_addr.sin_addr), clientIP, 32 - 1);
	spdlog::info("[Connect] Client IP : {} / SOCKET : {} || [unique_no:{}]", clientIP, (int)pPlayerSession->get_sock(), pPlayerSession->get_unique_no());
	return true;
}

void Epoll_Server::OnRecv(const int sock, const int ioSize)
{
	auto pPlayerSession = getSessionByNo(sock);

	// 쓰기를 위한 위치를 옮겨준다.
	if (!pPlayerSession->read_buffer().moveWritePos(ioSize))
	{
		spdlog::error("ReadBuffer Over Flow || [unique_no:{}]", pPlayerSession->get_unique_no());
	}

	PACKET_HEADER header;
	pPlayerSession->set_remainSize(0);
	while (pPlayerSession->read_buffer().getReadAbleSize() > 0) {

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

			/*spdlog::info("uniqueNo : {}, packet Size : {}, protocolBase : {}, protocolType : {}", 
				pPlayerSession->get_unique_no(), header.packet_len, protocolBase, header.packet_type);*/
			api.packet_Add(sock, pPlayerSession->get_unique_no(), pPlayerSession->read_buffer().getReadBuffer(), header.packet_len);

			// 읽기 완료 처리
			pPlayerSession->read_buffer().moveReadPos(header.packet_len);
			pPlayerSession->incr_remainSize((ioSize - header.packet_len));
		}


	}

}
