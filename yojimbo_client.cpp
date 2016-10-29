 /*
    Yojimbo Client/Server Network Protocol Library.
    
    Copyright © 2016, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "yojimbo_client.h"
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

namespace yojimbo
{
    const char * GetClientStateName( int clientState )
    {
        switch ( clientState )
        {
#if YOJIMBO_INSECURE_CONNECT
            case CLIENT_STATE_INSECURE_CONNECT_TIMEOUT:         return "insecure connect timeout";
#endif // #if YOJIMBO_INSECURE_CONNECT
            case CLIENT_STATE_PACKET_FACTORY_ERROR:             return "packet factory error";
            case CLIENT_STATE_MESSAGE_FACTORY_ERROR:            return "message factory error";
            case CLIENT_STATE_STREAM_ALLOCATOR_ERROR:           return "stream allocator error";
            case CLIENT_STATE_CONNECTION_REQUEST_TIMEOUT:       return "connection request timeout";
            case CLIENT_STATE_CHALLENGE_RESPONSE_TIMEOUT:       return "challenge response timeout";
            case CLIENT_STATE_CONNECTION_TIMEOUT:               return "connection timeout";
            case CLIENT_STATE_CONNECTION_ERROR:                 return "connection error";
            case CLIENT_STATE_CONNECTION_DENIED:                return "connection denied";
            case CLIENT_STATE_DISCONNECTED:                     return "disconnected";
#if YOJIMBO_INSECURE_CONNECT
            case CLIENT_STATE_SENDING_INSECURE_CONNECT:         return "sending insecure connect";
#endif // #if YOJIMBO_INSECURE_CONNECT
            case CLIENT_STATE_SENDING_CONNECTION_REQUEST:       return "sending connection request";
            case CLIENT_STATE_SENDING_CHALLENGE_RESPONSE:       return "sending challenge response";
            case CLIENT_STATE_CONNECTED:                        return "connected";
            default:
                assert( false );
                return "???";
        }
    }

    void Client::Defaults()
    {
        m_context = NULL;
        m_allocator = NULL;
        m_streamAllocator = NULL;
        m_transport = NULL;
        m_messageFactory = NULL;
        m_allocateConnection = false;
        m_connection = NULL;
        m_time = 0.0;
        m_connectTokenExpireTimestamp = 0;
        m_clientState = CLIENT_STATE_DISCONNECTED;
        m_clientIndex = -1;
        m_lastPacketSendTime = 0.0;
        m_lastPacketReceiveTime = 0.0;
        m_clientSalt = 0;
        m_sequence = 0;
    }

    Client::Client( Allocator & allocator, Transport & transport, const ClientServerConfig & config )
    {
        Defaults();
        m_allocator = &allocator;
        m_transport = &transport;
        m_config = config;
        m_config.connectionConfig.connectionPacketType = CLIENT_SERVER_PACKET_CONNECTION;
        m_allocateConnection = m_config.enableConnection;
    }

    Client::~Client()
    {
        // IMPORTANT: Please disconnect the client before destroying it
        assert( !IsConnected() );

        assert( m_allocator );

        YOJIMBO_DELETE( *m_allocator, Connection, m_connection );

        YOJIMBO_DELETE( *m_allocator, MessageFactory, m_messageFactory );

        YOJIMBO_DELETE( *m_allocator, Allocator, m_streamAllocator );

        YOJIMBO_DELETE( *m_allocator, ClientServerContext, m_context );

        m_messageFactory = NULL;
        m_transport = NULL;
        m_allocator = NULL;
    }

#if YOJIMBO_INSECURE_CONNECT

    void Client::InsecureConnect( const Address & address )
    {
        Disconnect();

        InitializeConnection();

        m_serverAddress = address;

        OnConnect( address );

        SetClientState( CLIENT_STATE_SENDING_INSECURE_CONNECT );

        const double time = GetTime();        

        m_lastPacketSendTime = time - 1.0f;
        m_lastPacketReceiveTime = time;

        RandomBytes( (uint8_t*) &m_clientSalt, sizeof( m_clientSalt ) );

        m_transport->ResetEncryptionMappings();
    }

#endif // #if YOJIMBO_INSECURE_CONNECT

    void Client::Connect( const Address & address, 
                          const uint8_t * connectTokenData, 
                          const uint8_t * connectTokenNonce,
                          const uint8_t * clientToServerKey,
                          const uint8_t * serverToClientKey,
                          uint64_t connectTokenExpireTimestamp )
    {
        Disconnect();

        InitializeConnection();

        m_serverAddress = address;

        SetEncryptedPacketTypes();

        OnConnect( address );

        SetClientState( CLIENT_STATE_SENDING_CONNECTION_REQUEST );

        const double time = GetTime();        

        m_lastPacketSendTime = time - 1.0f;
        m_lastPacketReceiveTime = time;
        memcpy( m_connectTokenData, connectTokenData, ConnectTokenBytes );
        memcpy( m_connectTokenNonce, connectTokenNonce, NonceBytes );

        m_transport->AddEncryptionMapping( m_serverAddress, clientToServerKey, serverToClientKey );

        m_connectTokenExpireTimestamp = connectTokenExpireTimestamp;
    }

    void Client::Disconnect( int clientState, bool sendDisconnectPacket )
    {
        assert( clientState <= CLIENT_STATE_DISCONNECTED );

        if ( m_clientState <= CLIENT_STATE_DISCONNECTED )
            return;

        if ( m_clientState != clientState )
            OnDisconnect();

        if ( sendDisconnectPacket && m_clientState > CLIENT_STATE_DISCONNECTED )
        {
            for ( int i = 0; i < m_config.numDisconnectPackets; ++i )
            {
                ConnectionDisconnectPacket * packet = (ConnectionDisconnectPacket*) m_transport->CreatePacket( CLIENT_SERVER_PACKET_CONNECTION_DISCONNECT );            

                if ( packet )
                {
                    SendPacketToServer_Internal( packet, true );
                }
            }
        }

        ResetConnectionData( clientState );

        m_transport->ResetEncryptionMappings();
    }

    Message * Client::CreateMessage( int type )
    {
        assert( m_messageFactory );
        return m_messageFactory->Create( type );
    }

    bool Client::CanSendMessage()
    {
        if ( !IsConnected() )
            return false;

        assert( m_messageFactory );
        assert( m_connection );
        
        return m_connection->CanSendMessage();
    }

    void Client::SendMessage( Message * message )
    {
        assert( IsConnected() );
        assert( m_messageFactory );
        assert( m_connection );
        m_connection->SendMessage( message );
    }

    Message * Client::ReceiveMessage()
    {
        assert( m_messageFactory );

        if ( !IsConnected() )
            return NULL;

        assert( m_connection );

        return m_connection->ReceiveMessage();
    }

    void Client::ReleaseMessage( Message * message )
    {
        assert( message );
        assert( m_messageFactory );
        m_messageFactory->Release( message );
    }

    MessageFactory & Client::GetMessageFactory()
    {
        assert( m_messageFactory );
        return *m_messageFactory;
    }

    bool Client::IsConnecting() const
    {
        return m_clientState > CLIENT_STATE_DISCONNECTED && m_clientState < CLIENT_STATE_CONNECTED;
    }

    bool Client::IsConnected() const
    {
        return m_clientState == CLIENT_STATE_CONNECTED;
    }

    bool Client::IsDisconnected() const
    {
        return m_clientState <= CLIENT_STATE_DISCONNECTED;
    }

    bool Client::ConnectionFailed() const
    {
        return m_clientState < CLIENT_STATE_DISCONNECTED;
    }

    ClientState Client::GetClientState() const
    { 
        return m_clientState;
    }

    void Client::SendPackets()
    {
        const double time = GetTime();

        switch ( m_clientState )
        {
#if YOJIMBO_INSECURE_CONNECT

            case CLIENT_STATE_SENDING_INSECURE_CONNECT:
            {
                if ( m_lastPacketSendTime + m_config.insecureConnectSendRate > time )
                    return;

                InsecureConnectPacket * packet = (InsecureConnectPacket*) m_transport->CreatePacket( CLIENT_SERVER_PACKET_INSECURE_CONNECT );
                if ( packet )
                {
                    packet->clientSalt = m_clientSalt;
                    SendPacketToServer_Internal( packet );
                }
            }
            break;

#endif // #if YOJIMBO_INSECURE_CONNECT

            case CLIENT_STATE_SENDING_CONNECTION_REQUEST:
            {
                if ( m_lastPacketSendTime + m_config.connectionRequestSendRate > time )
                    return;

                ConnectionRequestPacket * packet = (ConnectionRequestPacket*) m_transport->CreatePacket( CLIENT_SERVER_PACKET_CONNECTION_REQUEST );

                if ( packet )
                {
                    packet->connectTokenExpireTimestamp = m_connectTokenExpireTimestamp;
                    memcpy( packet->connectTokenData, m_connectTokenData, ConnectTokenBytes );
                    memcpy( packet->connectTokenNonce, m_connectTokenNonce, NonceBytes );

                    SendPacketToServer_Internal( packet );
                }
            }
            break;

            case CLIENT_STATE_SENDING_CHALLENGE_RESPONSE:
            {
                if ( m_lastPacketSendTime + m_config.connectionResponseSendRate > time )
                    return;

                ConnectionResponsePacket * packet = (ConnectionResponsePacket*) m_transport->CreatePacket( CLIENT_SERVER_PACKET_CONNECTION_RESPONSE );
                
                if ( packet )
                {
                    memcpy( packet->challengeTokenData, m_challengeTokenData, ChallengeTokenBytes );
                    memcpy( packet->challengeTokenNonce, m_challengeTokenNonce, NonceBytes );
                    
                    SendPacketToServer_Internal( packet );
                }
            }
            break;

            case CLIENT_STATE_CONNECTED:
            {
                if ( m_connection )
                {
                    ConnectionPacket * packet = m_connection->GeneratePacket();

                    if ( packet )
                    {
                        SendPacketToServer( packet );
                    }
                }

                if ( m_lastPacketSendTime + m_config.connectionHeartBeatRate <= time )
                {
                    ConnectionHeartBeatPacket * packet = (ConnectionHeartBeatPacket*) m_transport->CreatePacket( CLIENT_SERVER_PACKET_CONNECTION_HEARTBEAT );

                    if ( packet )
                    {
                        SendPacketToServer( packet );
                    }
                }
            }
            break;

            default:
                break;
        }
    }

    void Client::ReceivePackets()
    {
        while ( true )
        {
            Address address;
            uint64_t sequence;
            Packet * packet = m_transport->ReceivePacket( address, &sequence );
            if ( !packet )
                break;

            ProcessPacket( packet, address, sequence );

            packet->Destroy();
        }
    }

    void Client::CheckForTimeOut()
    {
        const double time = GetTime();

        switch ( m_clientState )
        {
#if YOJIMBO_INSECURE_CONNECT

            case CLIENT_STATE_SENDING_INSECURE_CONNECT:
            {
                if ( m_lastPacketReceiveTime + m_config.insecureConnectTimeOut < time )
                {
                    Disconnect( CLIENT_STATE_INSECURE_CONNECT_TIMEOUT, false );
                    return;
                }
            }
            break;

#endif // #if YOJIMBO_INSECURE_CONNECT

            case CLIENT_STATE_SENDING_CONNECTION_REQUEST:
            {
                if ( m_lastPacketReceiveTime + m_config.connectionRequestTimeOut < time )
                {
                    Disconnect( CLIENT_STATE_CONNECTION_REQUEST_TIMEOUT, false );
                    return;
                }
            }
            break;

            case CLIENT_STATE_SENDING_CHALLENGE_RESPONSE:
            {
                if ( m_lastPacketReceiveTime + m_config.challengeResponseTimeOut < time )
                {
                    Disconnect( CLIENT_STATE_CHALLENGE_RESPONSE_TIMEOUT, false );
                    return;
                }
            }
            break;

            case CLIENT_STATE_CONNECTED:
            {
                if ( m_lastPacketReceiveTime + m_config.connectionTimeOut < time )
                {
                    Disconnect( CLIENT_STATE_CONNECTION_TIMEOUT, false );
                    return;
                }
            }
            break;

            default:
                break;
        }
    }

    void Client::AdvanceTime( double time )
    {
        assert( time >= m_time );

        m_time = time;

        if ( m_streamAllocator && m_streamAllocator->GetError() )
        {
            Disconnect( CLIENT_STATE_STREAM_ALLOCATOR_ERROR, true );
            m_streamAllocator->ClearError();
            return;
        }

        if ( m_messageFactory && m_messageFactory->GetError() )
        {
            Disconnect( CLIENT_STATE_MESSAGE_FACTORY_ERROR, true );
            m_messageFactory->ClearError();
            return;
        }

        PacketFactory * packetFactory = m_transport->GetPacketFactory();
        if ( packetFactory && packetFactory->GetError() )
        {
            Disconnect( CLIENT_STATE_PACKET_FACTORY_ERROR, true );
            packetFactory->ClearError();
            return;
        }

        if ( m_connection )
        {
            if ( m_connection->GetError() )
            {
                Disconnect( CLIENT_STATE_CONNECTION_ERROR, true );
                return;
            }

            m_connection->AdvanceTime( time );
        }
    }

    double Client::GetTime() const
    {
        return m_time;
    }

    int Client::GetClientIndex() const
    {
        return m_clientIndex;
    }

    void Client::InitializeConnection()
    {
        if ( !m_streamAllocator )
        {
            m_streamAllocator = CreateStreamAllocator();

            m_transport->SetStreamAllocator( *m_streamAllocator );
        }

        if ( m_config.enableConnection )
        {
            if ( m_allocateConnection && !m_connection )
            {
                m_messageFactory = CreateMessageFactory();
                m_connection = YOJIMBO_NEW( *m_allocator, Connection, *m_allocator, *m_transport->GetPacketFactory(), *m_messageFactory, m_config.connectionConfig );
                m_connection->SetListener( this );
            }

            m_context = CreateContext();
            
            m_transport->SetContext( m_context );
        }
        else
        {
            m_transport->SetContext( NULL );
        }
    }

    void Client::SetEncryptedPacketTypes()
    {
        m_transport->EnablePacketEncryption();
        m_transport->DisableEncryptionForPacketType( CLIENT_SERVER_PACKET_CONNECTION_REQUEST );
    }

    Allocator * Client::CreateStreamAllocator()
    {
        return YOJIMBO_NEW( *m_allocator, DefaultAllocator );
    }

    PacketFactory * Client::CreatePacketFactory()
    {
        return YOJIMBO_NEW( *m_allocator, ClientServerPacketFactory, *m_allocator );
    }

    MessageFactory * Client::CreateMessageFactory()
    {
        assert( !"you need to override Client::CreateMessageFactory if you want to use messages" );
        return NULL;
    }

    ClientServerContext * Client::CreateContext()
    {
        ClientServerContext * context = YOJIMBO_NEW( *m_allocator, ClientServerContext );
        
        assert( context );
        assert( context->magic == ConnectionContextMagic );

        context->connectionConfig = &m_config.connectionConfig;

        context->messageFactory = &GetMessageFactory();
        
        return context;
    }

    void Client::SetClientState( int clientState )
    {
        const int previous = m_clientState;

        m_clientState = (ClientState) clientState;

        if ( clientState != previous )
            OnClientStateChange( previous, clientState );
    }

    void Client::ResetConnectionData( int clientState )
    {
        assert( m_transport );
        m_clientIndex = -1;
        m_serverAddress = Address();
        SetClientState( clientState );
        m_lastPacketSendTime = -1000.0;
        m_lastPacketReceiveTime = -1000.0;
        memset( m_connectTokenData, 0, ConnectTokenBytes );
        memset( m_connectTokenNonce, 0, NonceBytes );
        memset( m_challengeTokenData, 0, ChallengeTokenBytes );
        memset( m_challengeTokenNonce, 0, NonceBytes );
        m_transport->ResetEncryptionMappings();
        m_sequence = 0;
#if YOJIMBO_INSECURE_CONNECT
        m_clientSalt = 0;
#endif // #if YOJIMBO_INSECURE_CONNECT
        if ( m_connection )
        {
            m_connection->Reset();
        }
    }

    void Client::SendPacketToServer( Packet * packet )
    {
        assert( packet );
        assert( m_serverAddress.IsValid() );

        if ( !IsConnected() )
        {
            packet->Destroy();
            return;
        }

        SendPacketToServer_Internal( packet, false );
    }

    void Client::SendPacketToServer_Internal( Packet * packet, bool immediate )
    {
        assert( packet );
        assert( m_clientState > CLIENT_STATE_DISCONNECTED );
        assert( m_serverAddress.IsValid() );

        m_transport->SendPacket( m_serverAddress, packet, ++m_sequence, immediate );

        OnPacketSent( packet->GetType(), m_serverAddress, immediate );

        m_lastPacketSendTime = GetTime();
    }

    void Client::ProcessConnectionDenied( const ConnectionDeniedPacket & /*packet*/, const Address & address )
    {
        if ( m_clientState != CLIENT_STATE_SENDING_CONNECTION_REQUEST )
            return;

        if ( address != m_serverAddress )
            return;

        SetClientState( CLIENT_STATE_CONNECTION_DENIED );
    }

    void Client::ProcessConnectionChallenge( const ConnectionChallengePacket & packet, const Address & address )
    {
        if ( m_clientState != CLIENT_STATE_SENDING_CONNECTION_REQUEST )
            return;

        if ( address != m_serverAddress )
            return;

        memcpy( m_challengeTokenData, packet.challengeTokenData, ChallengeTokenBytes );
        memcpy( m_challengeTokenNonce, packet.challengeTokenNonce, NonceBytes );

        SetClientState( CLIENT_STATE_SENDING_CHALLENGE_RESPONSE );

        const double time = GetTime();

        m_lastPacketReceiveTime = time;
    }

    bool Client::IsPendingConnect()
    {
#if YOJIMBO_INSECURE_CONNECT
        return m_clientState == CLIENT_STATE_SENDING_CHALLENGE_RESPONSE || m_clientState == CLIENT_STATE_SENDING_INSECURE_CONNECT;
#else // #if YOJIMBO_INSECURE_CONNECT
        return m_clientState == CLIENT_STATE_SENDING_CHALLENGE_RESPONSE;
#endif // #if YOJIMBO_INSECURE_CONNECT
    }

    void Client::CompletePendingConnect( int clientIndex )
    {
        if ( m_clientState == CLIENT_STATE_SENDING_CHALLENGE_RESPONSE )
        {
            m_clientIndex = clientIndex;

            memset( m_connectTokenData, 0, ConnectTokenBytes );
            memset( m_connectTokenNonce, 0, NonceBytes );
            memset( m_challengeTokenData, 0, ChallengeTokenBytes );
            memset( m_challengeTokenNonce, 0, NonceBytes );

            SetClientState( CLIENT_STATE_CONNECTED );
        }

#if YOJIMBO_INSECURE_CONNECT

        if ( m_clientState == CLIENT_STATE_SENDING_INSECURE_CONNECT )
        {
            m_clientIndex = clientIndex;

            SetClientState( CLIENT_STATE_CONNECTED );
        }

#endif // #if YOJIMBO_INSECURE_CONNECT
    }

    void Client::ProcessConnectionHeartBeat( const ConnectionHeartBeatPacket & packet, const Address & address )
    {
        if ( !IsPendingConnect() && !IsConnected() )
            return;

        if ( address != m_serverAddress )
            return;

        if ( IsPendingConnect() )
            CompletePendingConnect( packet.clientIndex );

        m_lastPacketReceiveTime = GetTime();
    }

    void Client::ProcessConnectionDisconnect( const ConnectionDisconnectPacket & /*packet*/, const Address & address )
    {
        if ( m_clientState != CLIENT_STATE_CONNECTED )
            return;

        if ( address != m_serverAddress )
            return;

        Disconnect( CLIENT_STATE_DISCONNECTED, false );
    }

    void Client::ProcessConnectionPacket( ConnectionPacket & packet, const Address & address )
    {
        if ( !IsConnected() )
            return;

        if ( address != m_serverAddress )
            return;

        if ( m_connection )
            m_connection->ProcessPacket( &packet );

        m_lastPacketReceiveTime = GetTime();
    }

    void Client::ProcessPacket( Packet * packet, const Address & address, uint64_t sequence )
    {
        OnPacketReceived( packet->GetType(), address, sequence );
        
        switch ( packet->GetType() )
        {
            case CLIENT_SERVER_PACKET_CONNECTION_DENIED:
                ProcessConnectionDenied( *(ConnectionDeniedPacket*)packet, address );
                return;

            case CLIENT_SERVER_PACKET_CONNECTION_CHALLENGE:
                ProcessConnectionChallenge( *(ConnectionChallengePacket*)packet, address );
                return;

            case CLIENT_SERVER_PACKET_CONNECTION_HEARTBEAT:
                ProcessConnectionHeartBeat( *(ConnectionHeartBeatPacket*)packet, address );
                return;

            case CLIENT_SERVER_PACKET_CONNECTION_DISCONNECT:
                ProcessConnectionDisconnect( *(ConnectionDisconnectPacket*)packet, address );
                return;

            case CLIENT_SERVER_PACKET_CONNECTION:
                ProcessConnectionPacket( *(ConnectionPacket*)packet, address );
                return;

            default:
                break;
        }

        if ( !IsConnected() )
            return;

        if ( address != m_serverAddress )
            return;

        if ( !ProcessGamePacket( packet, sequence ) )
            return;

        m_lastPacketReceiveTime = GetTime();
    }
}
