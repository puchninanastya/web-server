#include "worker_thread.hpp"

using namespace monzza::worker;

using HttpPair = std::pair<monzza::http::HttpRequestParser*, monzza::http::HttpResponseSerializer*>;
using TcpServerExchangeSocketPair = std::pair<cpl::TcpServerExchangeSocket*, cpl::Event*>;

bool WorkerThreadServiceMessage::setResponseType( WorkerThreadServiceMessage::ResponseType responseType ) {
    responseType_ = responseType;
    return true;
}

WorkerThreadServiceMessage::ResponseType WorkerThreadServiceMessage::getResponseType() const {
    return responseType_;
}

bool WorkerThreadServiceMessage::setCommandType( WorkerThreadServiceMessage::CommandType commandType ) {
    commandType_ = commandType;
    return true;
}

WorkerThreadServiceMessage::CommandType WorkerThreadServiceMessage::getCommandType() const {
    return commandType_;
}

bool WorkerThreadServiceMessage::setNewConnection( cpl::TcpServerExchangeSocket* tcpServerExchangeSocket ) {
    tcpServerExchangeSocket_ = tcpServerExchangeSocket;
    return true;
}

cpl::TcpServerExchangeSocket* WorkerThreadServiceMessage::getNewConnection() const {
    return tcpServerExchangeSocket_;
}

WorkerThread::WorkerThread() {
    breakThreadLoop_  = false;
    bufPtr_ = new uint8_t[ 8192 ];
    bufSize_ = 8192;
    events_.push_back( inputServiceMessages_.getEventHandle() );
}

WorkerThread::~WorkerThread() {
    delete[] bufPtr_;
}

bool WorkerThread::initialize( monzza::logger::Logger* logger,
                               monzza::table::Table* table,
                               WorkerSettings* workerSettings )
{
    if ( ( logger == nullptr ) || ( table == nullptr ) || ( workerSettings == nullptr ) ) {
        return false;
    }

    setLogger( logger );

    std::string moduleName( "WorkerThread " );
    moduleName += std::to_string( workerSettings->getId() );
    setModuleName( moduleName );

    table_ = table;
    workerSettings_ = workerSettings;

    httpFileSender_.initialize( getLogger(), getModuleName(), workerSettings->getDocumentRoot() );

    return true;
}

cpl::Event* WorkerThread::getNewOutputServiceMessageEvent() {
    return ( outputServiceMessages_.getEventHandle() );
}

void WorkerThread::addInputServiceMessage( WorkerThreadServiceMessage* workerThreadServiceMessage ) {
    inputServiceMessages_.push( workerThreadServiceMessage );
}

WorkerThreadServiceMessage* WorkerThread::getOutputServiceMessage() {
    WorkerThreadServiceMessage* workerThreadServiceMessage;
    return ( outputServiceMessages_.tryPop( workerThreadServiceMessage ) ? workerThreadServiceMessage : nullptr );
}

void WorkerThread::operator()() {
    uint32_t waitResult = 0;

    notificationMsg( "Module started." );

    debugMsg( "Updating information about worker." );

    table_->updateWorkerInformation( workerSettings_->getId(),
                                     ( uint32_t )tcpConnections_.size(), 0  );

    debugMsg( "Updated information about worker." );

    while ( !breakThreadLoop_ ) {
        waitResult = cpl::EventExpectant::waitForEvents( &events_, false, CPL_EE_WFE_INFINITE_WAIT );
        switch ( waitResult ) {
            case 0:  // New service message.
                processInputServiceMessage();
                break;
            default: // Tcp server exchange socket event.
                if ( waitResult > 0 ) {
                    processTcpServerExchangeSocketEvent( waitResult );
                }
                break;
        }
    }

    notificationMsg( "Module stopped." );
}

void WorkerThread::processInputServiceMessage() {
    WorkerThreadServiceMessage* workerThreadServiceMessage;
    inputServiceMessages_.tryPop( workerThreadServiceMessage );

    bool needToSendResponseMessage = false;
    if ( workerThreadServiceMessage->getCommandType() == WorkerThreadServiceMessage::CommandType::STOP ) {
        needToSendResponseMessage = true;
    }

    serviceMsgHandler(workerThreadServiceMessage);

    if ( needToSendResponseMessage ) {
        outputServiceMessages_.push(workerThreadServiceMessage);
    }
}

void WorkerThread::serviceMsgHandler( WorkerThreadServiceMessage* workerThreadServiceMessage ) {
    if ( workerThreadServiceMessage == nullptr ) {
        return;
    }

    switch ( workerThreadServiceMessage->getCommandType() ) {
        case WorkerThreadServiceMessage::CommandType ::NEW_CONNECTION:
            newConnectionServiceMsgHandler( workerThreadServiceMessage );
            break;
        case WorkerThreadServiceMessage::CommandType::STOP:
            stopServiceMsgHandler( workerThreadServiceMessage );
            break;
    }
}

void WorkerThread::newConnectionServiceMsgHandler( WorkerThreadServiceMessage* workerThreadServiceMessage )
{
    notificationMsg( "Adding new connection." );

    cpl::TcpServerExchangeSocket* tcpServerExchangeSocket = workerThreadServiceMessage->getNewConnection();
    auto tcpServerExchangeSocketEvent = new cpl::Event;
    tcpServerExchangeSocketEvent->initializeEvent( *( tcpServerExchangeSocket ), CPL_SOCKET_EVENT_TYPE_READ );

    TcpConnection tcpConnection;
    tcpConnection.tcpServerExchangeSocket_ = tcpServerExchangeSocket;
    tcpConnection.tcpServerExchangeSocketEvent_ = tcpServerExchangeSocketEvent;

    auto httpRequestParser = new http::HttpRequestParser();
    httpRequestParser->initialize( getLogger(), getModuleName() );

    auto httpResponseSerializer = new http::HttpResponseSerializer();
    httpResponseSerializer->initialize( getLogger(), getModuleName() );

    tcpConnection.httpRequestParser_ = httpRequestParser;
    tcpConnection.httpResponseSerializer_ = httpResponseSerializer;

    tcpConnections_.push_back( tcpConnection );
    events_.push_back( tcpServerExchangeSocketEvent );

    notificationMsg( "Added new connection." );

    debugMsg( "Updating information about worker." );

    table_->updateWorkerInformation( workerSettings_->getId(),
                                     ( uint32_t )tcpConnections_.size(), 0 );

    debugMsg( "Updated information about worker." );

    delete workerThreadServiceMessage;
}

void WorkerThread::stopServiceMsgHandler( WorkerThreadServiceMessage* workerThreadServiceMessage ) {
    workerThreadServiceMessage->setResponseType( WorkerThreadServiceMessage::ResponseType::SUCCESSFUL );
    events_.clear();

    for ( uint32_t i = 0; i < tcpConnections_.size(); i++ ) {
        tcpConnections_[ i ].tcpServerExchangeSocket_->close();
        delete tcpConnections_[ i ].tcpServerExchangeSocket_;
        delete tcpConnections_[ i ].tcpServerExchangeSocketEvent_;
        delete tcpConnections_[ i ].httpRequestParser_;
        delete tcpConnections_[ i ].httpResponseSerializer_;
    }

    breakThreadLoop_ = true;
}

void WorkerThread::processTcpServerExchangeSocketEvent( uint32_t index ) {
    cpl::TcpServerExchangeSocket* tcpServerExchangeSocket;

    uint32_t connectionIndex = index - 1;

    tcpServerExchangeSocket = ( tcpConnections_.at( connectionIndex ) ).tcpServerExchangeSocket_;
    bool sendingFile = ( tcpConnections_.at( connectionIndex ) ).httpFileDescription_.sending;

    if ( !sendingFile ) {
        int32_t result = tcpServerExchangeSocket->receive( bufPtr_, bufSize_ );
        if ( result > 0 ) {
            notificationMsg( "Processing connection request." );
            if ( processSocketData( ( connectionIndex ), result ) ) {
                if ( !tcpConnections_[ connectionIndex ].httpFileDescription_.sending ) {
                    clearDataForConnection( index );
                    notificationMsg( "Processed connection request." );
                }
                else {
                    setLowPriorityForConnection( connectionIndex );
                    notificationMsg( "Need to send more data." );
                }
            }

        }
        else {
            clearDataForConnection( index );
        }
    }
    else {
        // trick to check connection.
        uint8_t tempBuf[ 1 ];
        int result = write( tcpServerExchangeSocket->getPlatformSocket(), tempBuf, 1 );

        if ( result >= 0 ) {
            notificationMsg( "Trying to send rest of the file." );
            tryToSendTheRestOfTheFile( connectionIndex );

            if ( tcpConnections_[ connectionIndex ].httpFileDescription_.sending ) {
                notificationMsg( "Send part of the file." );
                setLowPriorityForConnection( connectionIndex );
            }
            else {
                notificationMsg( "Send last part of the file." );
                clearDataForConnection( index );
            }
        }
        else {
            notificationMsg( "Clearing data for connection." );
            clearDataForConnection( index );
            notificationMsg( "Cleared data for connection." );
        }
    }
}

bool WorkerThread::processSocketData( uint32_t connectionIndex, int32_t readSize ) {
    auto httpParser = ( tcpConnections_[ connectionIndex ] ).httpRequestParser_;
    auto tcpExchangeSocket = ( tcpConnections_[ connectionIndex ] ).tcpServerExchangeSocket_;

    http::HttpRequest* request = nullptr;

    if ( !httpParser->addDataAndParse( bufPtr_, ( uint16_t )readSize ) ) {
        notificationMsg( "Request parsing failed for connection index " + std::to_string( connectionIndex ) );
        return false;
    }

    debugMsg( "Request parsing completed successfully." );

    request = httpParser->getHttpRequest();

    if ( !request ) {
        std::string strToSend = "HTTP/1.1 500 Failed\r\nServer: Monzza\r\nConnection: Closed\r\n\r\n";
        tcpExchangeSocket->send( ( uint8_t* )strToSend.c_str(), ( uint16_t )strToSend.length() );
        debugMsg( "Response has been sent for connection index " + std::to_string( connectionIndex ) );
        return true;
    }

    prepareAndSendHttpRequestForConnection( connectionIndex, request );

    return true;
}

bool WorkerThread::prepareAndSendHttpRequestForConnection( uint32_t connectionIndex,
                                                           monzza::http::HttpRequest* request )
{
    auto httpSerializer = ( tcpConnections_[ connectionIndex ] ).httpResponseSerializer_;
    auto tcpExchangeSocket = ( tcpConnections_[ connectionIndex ] ).tcpServerExchangeSocket_;

    monzza::http::HttpFileDescription httpFileDescription;

    switch ( request->getHttpMethod() ) {
        case monzza::http::HttpRequestMethod::GET:
        case monzza::http::HttpRequestMethod::HEAD: {
            httpFileDescription = httpFileSender_.getFileDescription( request->getUri() );

            switch ( httpFileDescription.httpFileReachability ) {
                case monzza::http::HttpFileReachability::EXISTS:
                    httpSerializer->createHttpResponseForExistingFile( httpFileDescription );
                    break;
                case monzza::http::HttpFileReachability::NOT_EXISTS:
                    httpSerializer->createHttpResponseForNotFound();
                    break;
                case monzza::http::HttpFileReachability::ACCESS_DENIED:
                    httpSerializer->createHttpResponseForForbidden();
                    break;
            }
            break;
        }
        case monzza::http::HttpRequestMethod::POST:
            httpSerializer->createHttpResponseForNotAllowed();
            break;
        default:
            httpSerializer->createHttpResponseForNotAllowed();
            break;
    }

    uint32_t responseBufSize = httpSerializer->getSerializedHttpResponseSize();

    if ( responseBufSize ) {
        auto responseBuffer = new uint8_t[ responseBufSize ];
        memset( responseBuffer, 0, responseBufSize );
        if ( httpSerializer->getSerializedHttpResponse( responseBuffer, responseBufSize ) ) {
            tcpExchangeSocket->send( responseBuffer, static_cast<uint16_t>( responseBufSize ) );
            delete[] responseBuffer;
        }

        if ( request->getHttpMethod() == monzza::http::HttpRequestMethod::GET &&
             httpFileDescription.httpFileReachability == monzza::http::HttpFileReachability::EXISTS )
        {
            httpFileSender_.sendFileThroughSocket( tcpExchangeSocket, httpFileDescription );
            tcpConnections_[ connectionIndex ].httpFileDescription_ = httpFileDescription;
        }
    }
}

bool WorkerThread::tryToSendTheRestOfTheFile( uint32_t connectionIndex ) {
    return ( httpFileSender_.sendFileThroughSocket( tcpConnections_[ connectionIndex ].tcpServerExchangeSocket_,
                                                    tcpConnections_[ connectionIndex ].httpFileDescription_ ) );
}

void WorkerThread::setLowPriorityForConnection( uint32_t connectionIndex ) {
    TcpConnection tcpConnection = tcpConnections_[ connectionIndex ];
    cpl::TcpServerExchangeSocket* tcpServerExchangeSocket;
    tcpServerExchangeSocket = tcpConnections_[ connectionIndex ].tcpServerExchangeSocket_;
    cpl::Event* tcpSocketEvent = events_[ connectionIndex + 1 ];

    tcpSocketEvent->initializeEvent( *( tcpServerExchangeSocket ), CPL_SOCKET_EVENT_TYPE_WRITE );

    tcpConnections_.erase( tcpConnections_.begin() + ( connectionIndex ) );
    events_.erase( events_.begin() + ( connectionIndex + 1 ) );
    tcpConnections_.push_back( tcpConnection );
    events_.push_back( tcpSocketEvent );
}

void WorkerThread::clearDataForConnection( uint32_t index ) {
    notificationMsg( "Deleting connection." );

    delete tcpConnections_[ index - 1 ].httpRequestParser_;
    delete tcpConnections_[ index - 1 ].httpResponseSerializer_;
    tcpConnections_[ index - 1 ].tcpServerExchangeSocket_->close();
    delete tcpConnections_[ index - 1 ].tcpServerExchangeSocket_;
    delete tcpConnections_[ index - 1 ].tcpServerExchangeSocketEvent_;

    tcpConnections_.erase( tcpConnections_.begin() + ( index - 1 ) );
    events_.erase( events_.begin() + index );

    notificationMsg( "Connection deleted." );

    debugMsg( "Updating information about worker." );

    table_->updateWorkerInformation( workerSettings_->getId(),
                                     ( uint32_t )tcpConnections_.size(), 0 );

    debugMsg( "Updated information about worker." );
}