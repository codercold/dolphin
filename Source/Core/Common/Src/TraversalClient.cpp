// This file is public domain, in case it's useful to anyone. -comex

#include "TraversalClient.h"
#include "enet/enet.h"

static void GetRandomishBytes(u8* buf, size_t size)
{
	// We don't need high quality random numbers (which might not be available),
	// just non-repeating numbers!
	srand(enet_time_get());
	for (size_t i = 0; i < size; i++)
		buf[i] = rand() & 0xff;
}

TraversalClient::TraversalClient(NetHost* netHost, const std::string& server)
{
	m_NetHost = netHost;
	m_Server = server;
	m_Client = NULL;

	m_NetHost->m_TraversalClient = this;

	Reset();

	ReconnectToServer();
}

TraversalClient::~TraversalClient()
{
	if (m_NetHost)
	{
		m_NetHost->RunOnThreadSync([&]() {
			ASSUME_ON(NET);
			m_NetHost->m_TraversalClient = NULL;
		});
	}
}

void TraversalClient::ReconnectToServer()
{
	m_Server = "vps.qoid.us"; // XXX
	if (enet_address_set_host(&m_ServerAddress, m_Server.c_str()))
	{
		m_NetHost->RunOnThread([=]() {
			ASSUME_ON(NET);
			OnFailure(BadHost);
		});
		return;
	}
	m_ServerAddress.port = 6262;

	m_State = Connecting;

	TraversalPacket hello = {0};
	hello.type = TraversalPacketHelloFromClient;
	hello.helloFromClient.protoVersion = TraversalProtoVersion;
	m_NetHost->RunOnThread([=]() {
		ASSUME_ON(NET);
		SendTraversalPacket(hello);
		if (m_Client)
			m_Client->OnTraversalStateChanged();
	});
}

static ENetAddress MakeENetAddress(TraversalInetAddress* address)
{
	ENetAddress eaddr;
	if (address->isIPV6)
	{
		eaddr.port = 0; // no support yet :(
	}
	else
	{
		eaddr.host = address->address[0];
		eaddr.port = ntohs(address->port);
	}
	return eaddr;
}

void TraversalClient::ConnectToClient(const std::string& host)
{
	if (host.size() > sizeof(TraversalHostId))
	{
		PanicAlert("host too long");
		return;
	}
	TraversalPacket packet = {0};
	packet.type = TraversalPacketConnectPlease;
	memcpy(packet.connectPlease.hostId.data(), host.c_str(), host.size());
	m_ConnectRequestId = SendTraversalPacket(packet);
	m_PendingConnect = true;
}

bool TraversalClient::TestPacket(u8* data, size_t size, ENetAddress* from)
{
	if (from->host == m_ServerAddress.host &&
	    from->port == m_ServerAddress.port)
	{
		if (size < sizeof(TraversalPacket))
		{
			ERROR_LOG(NETPLAY, "Received too-short traversal packet.");
		}
		else
		{
			HandleServerPacket((TraversalPacket*) data);
			return true;
		}
	}
	return false;
}

void TraversalClient::HandleServerPacket(TraversalPacket* packet)
{
	u8 ok = 1;
	switch (packet->type)
	{
	case TraversalPacketAck:
		if (!packet->ack.ok)
		{
			OnFailure(ServerForgotAboutUs);
			break;
		}
		for (auto it = m_OutgoingTraversalPackets.begin(); it != m_OutgoingTraversalPackets.end(); ++it)
		{
			if (it->packet.requestId == packet->requestId)
			{
				m_OutgoingTraversalPackets.erase(it);
				break;
			}
		}
		break;
	case TraversalPacketHelloFromServer:
		if (m_State != Connecting)
			break;
		if (!packet->helloFromServer.ok)
		{
			OnFailure(VersionTooOld);
			break;
		}
		m_HostId = packet->helloFromServer.yourHostId;
		m_State = Connected;
		if (m_Client)
			m_Client->OnTraversalStateChanged();
		break;
	case TraversalPacketPleaseSendPacket:
		{
		// security is overrated.
		ENetAddress addr = MakeENetAddress(&packet->pleaseSendPacket.address);
		if (addr.port != 0)
		{
			char message[] = "Hello from Dolphin Netplay...";
			ENetBuffer buf;
			buf.data = message;
			buf.dataLength = sizeof(message) - 1;
			enet_socket_send(m_NetHost->m_Host->socket, &addr, &buf, 1);

		}
		else
		{
			// invalid IPV6
			ok = 0;
		}
		break;
		}
	case TraversalPacketConnectReady:
	case TraversalPacketConnectFailed:
		{

		if (!m_PendingConnect || packet->connectReady.requestId != m_ConnectRequestId)
			break;

		m_PendingConnect = false;

		if (!m_Client)
			break;

		if (packet->type == TraversalPacketConnectReady)
			m_Client->OnConnectReady(MakeENetAddress(&packet->connectReady.address));
		else
			m_Client->OnConnectFailed(packet->connectFailed.reason);

		break;
		}
	default:
		WARN_LOG(NETPLAY, "Received unknown packet with type %d", packet->type);
		break;
	}
	if (packet->type != TraversalPacketAck)
	{
		TraversalPacket ack = {0};
		ack.type = TraversalPacketAck;
		ack.requestId = packet->requestId;
		ack.ack.ok = ok;

		ENetBuffer buf;
		buf.data = &ack;
		buf.dataLength = sizeof(ack);
		if (enet_socket_send(m_NetHost->m_Host->socket, &m_ServerAddress, &buf, 1) == -1)
			OnFailure(SocketSendError);
	}
}

void TraversalClient::OnFailure(int reason)
{
	m_State = Failure;
	m_FailureReason = reason;
	if (m_Client)
		m_Client->OnTraversalStateChanged();
}

void TraversalClient::ResendPacket(OutgoingTraversalPacketInfo* info)
{
	info->sendTime = enet_time_get();
	info->tries++;
	ENetBuffer buf;
	buf.data = &info->packet;
	buf.dataLength = sizeof(info->packet);
	if (enet_socket_send(m_NetHost->m_Host->socket, &m_ServerAddress, &buf, 1) == -1)
		OnFailure(SocketSendError);
}

void TraversalClient::HandleResends()
{
	enet_uint32 now = enet_time_get();
	for (auto& tpi : m_OutgoingTraversalPackets)
	{
		if (now - tpi.sendTime >= (u32) (300 * tpi.tries))
		{
			if (tpi.tries >= 5)
			{
				OnFailure(ResendTimeout);
				m_OutgoingTraversalPackets.clear();
				break;
			}
			else
			{
				ResendPacket(&tpi);
			}
		}
	}
	HandlePing();
}

void TraversalClient::HandlePing()
{
	enet_uint32 now = enet_time_get();
	if (m_State == Connected && now - m_PingTime >= 500)
	{
		TraversalPacket ping = {0};
		ping.type = TraversalPacketPing;
		ping.ping.hostId = m_HostId;
		SendTraversalPacket(ping);
		m_PingTime = now;
	}
}

TraversalRequestId TraversalClient::SendTraversalPacket(const TraversalPacket& packet)
{
	OutgoingTraversalPacketInfo info;
	info.packet = packet;
	GetRandomishBytes((u8*) &info.packet.requestId, sizeof(info.packet.requestId));
	info.tries = 0;
	m_OutgoingTraversalPackets.push_back(info);
	ResendPacket(&m_OutgoingTraversalPackets.back());
	return info.packet.requestId;
}

void TraversalClient::Reset()
{
	m_PendingConnect = false;
	m_Client = NULL;
}

std::unique_ptr<TraversalClient> g_TraversalClient;
std::unique_ptr<NetHost> g_MainNetHost;

// The settings at the previous TraversalClient reset - notably, we
// need to know not just what port it's on, but whether it was
// explicitly requested.
static std::string g_OldServer;
static u16 g_OldPort;

bool EnsureTraversalClient(const std::string& server, u16 port)
{
	if (!g_TraversalClient || server != g_OldServer || port != g_OldPort)
	{
		g_OldServer = server;
		g_OldPort = port;

		g_MainNetHost.reset(new NetHost(NetHost::DefaultPeerCount, port));
		if (!g_MainNetHost->m_Host)
		{
			g_MainNetHost.reset();
			return false;
		}

		g_TraversalClient.reset(new TraversalClient(g_MainNetHost.get(), server));
	}
	return true;
}

void ReleaseTraversalClient()
{
	if (!g_TraversalClient)
		return;

	if (g_OldPort != 0)
	{
		// If we were listening at a specific port, kill the
		// TraversalClient to avoid hanging on to the port.
		g_TraversalClient.reset();
		g_MainNetHost.reset();
	}
	else
	{
		// Reset any pending connection attempts.
		g_TraversalClient->Reset();
	}
}
