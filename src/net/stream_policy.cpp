// Copyright (c) 2020 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include <net/net.h>
#include <net/stream_policy.h>
#include <logging.h>

namespace
{
    // Classify messages we consider to be block related
    bool IsBlockMsg(const std::string& cmd)
    {
        return cmd == NetMsgType::BLOCK ||
               cmd == NetMsgType::CMPCTBLOCK ||
               cmd == NetMsgType::BLOCKTXN ||
               cmd == NetMsgType::GETBLOCKTXN;
    }

    // Classify msgs we consider high priority
    bool IsHighPriorityMsg(const std::string& cmd)
    {
        return cmd == NetMsgType::PING ||
               cmd == NetMsgType::PONG ||
               IsBlockMsg(cmd);
    }
}

// Initialise policy names
const std::string DefaultStreamPolicy::POLICY_NAME { "Default" };
const std::string BlockPriorityStreamPolicy::POLICY_NAME { "BlockPriority" };


/*****************************/
/** The DefaultStreamPolicy **/
/*****************************/

bool DefaultStreamPolicy::GetNextMessage(StreamMap& streams, std::list<CNetMessage>& msg)
{
    // Check we have a stream available (if we do we will have the GENERAL stream)
    if(streams.size() > 0)
    {
        return streams[StreamType::GENERAL]->GetNextMessage(msg);
    }

    return false;
}

void DefaultStreamPolicy::ServiceSockets(StreamMap& streams, fd_set& setRecv,
    fd_set& setSend, fd_set& setError, const Config& config, bool& gotNewMsgs,
    size_t& bytesRecv, size_t& bytesSent)
{
    // Check we have a stream available (if we do we will have the GENERAL stream)
    if(streams.size() > 0)
    {   
        streams[StreamType::GENERAL]->ServiceSocket(setRecv, setSend, setError, config, gotNewMsgs, bytesRecv, bytesSent);
    }
}

size_t DefaultStreamPolicy::PushMessage(StreamMap& streams, StreamType streamType,
    std::vector<uint8_t>&& serialisedHeader, CSerializedNetMsg&& msg,
    size_t nPayloadLength, size_t nTotalSize)
{
    // Check we have a stream available (if we do we will have the GENERAL stream)
    if(streams.size() > 0)
    {
        return streams[StreamType::GENERAL]->PushMessage(std::move(serialisedHeader), std::move(msg), nPayloadLength, nTotalSize);
    }

    throw std::runtime_error("DefaultStreamPolicy has no stream available for sending");
}


/***********************************/
/** The BlockPriorityStreamPolicy **/
/***********************************/

void BlockPriorityStreamPolicy::SetupStreams(CConnman& connman, const CAddress& peerAddr,
    const AssociationIDPtr& assocID)
{
    LogPrint(BCLog::NET, "BlockPriorityStreamPolicy opening required streams\n");
    connman.QueueNewStream(peerAddr, StreamType::DATA1, assocID, GetPolicyName());
}

bool BlockPriorityStreamPolicy::GetNextMessage(StreamMap& streams, std::list<CNetMessage>& msg)
{
    // Look for messages from streams in order of priority
    if(streams.count(StreamType::DATA1) == 1)
    {
        // Check highest priority DATA1 stream
        bool moreMsgs { streams[StreamType::DATA1]->GetNextMessage(msg) };
        if(msg.size() > 0)
        {
            return moreMsgs;
        }
    }

    if(streams.count(StreamType::GENERAL) == 1)
    {
        // Check lowest priority GENERAL stream
        return streams[StreamType::GENERAL]->GetNextMessage(msg);
    }

    return false;
}

void BlockPriorityStreamPolicy::ServiceSockets(StreamMap& streams, fd_set& setRecv,
    fd_set& setSend, fd_set& setError, const Config& config, bool& gotNewMsgs,
    size_t& bytesRecv, size_t& bytesSent)
{
    // Service each stream socket
    for(auto& stream : streams)
    {   
        size_t streamBytesRecv {0};
        size_t streamBytesSent {0};
        stream.second->ServiceSocket(setRecv, setSend, setError, config, gotNewMsgs,
            streamBytesRecv, streamBytesSent);
        bytesRecv += streamBytesRecv;
        bytesSent += streamBytesSent;
    }
}

size_t BlockPriorityStreamPolicy::PushMessage(StreamMap& streams, StreamType streamType,
    std::vector<uint8_t>&& serialisedHeader, CSerializedNetMsg&& msg,
    size_t nPayloadLength, size_t nTotalSize)
{
    // Have we been told which stream to use?
    bool exactMatch { streamType != StreamType::UNKNOWN };

    // If we haven't been told which stream to use, decide which we would prefer
    if(!exactMatch)
    {
        const std::string& cmd { msg.Command() };
        if(IsHighPriorityMsg(cmd))
        {
            // Pings, pongs and block msgs are sent over the high priority DATA1 stream if we have it
            streamType = StreamType::DATA1;
        }
        else
        {
            // Send over the GENERAL stream
            streamType = StreamType::GENERAL;
        }
    }

    // Find the appropriate stream
    StreamPtr destStream {nullptr};
    for(const auto& [type, stream] : streams)
    {
        if(type == streamType)
        {
            // Got the requested stream type
            destStream = stream;
            break;
        }
        else if(!exactMatch && type == StreamType::GENERAL)
        {
            // Can always send anything over a GENERAL stream
            destStream = stream;
        }
    }

    // If we found a stream, send
    if(!destStream)
    {
        std::stringstream err {};
        err << "No stream avilable of type " << enum_cast<std::string>(streamType)
            << " for message of type " << msg.Command();
        throw std::runtime_error(err.str());
    }

    return destStream->PushMessage(std::move(serialisedHeader), std::move(msg), nPayloadLength, nTotalSize);
}

