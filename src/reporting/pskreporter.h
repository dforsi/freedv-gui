#if !defined(PSK_REPORTER_H)
#define PSK_REPORTER_H

#include <mutex>
#include "IReporter.h"

struct SenderRecord
{
    std::string callsign;
    uint64_t frequency;
    char snr;
    std::string mode;
    char infoSource;
    int flowTimeSeconds;
    
    SenderRecord(std::string callsign, uint64_t frequency, char snr);
    
    int recordSize();    
    void encode(char* buf);
};

class PskReporter : public IReporter
{
public:
    PskReporter(std::string callsign, std::string gridSquare, std::string software);
    virtual ~PskReporter();
    
    virtual void addReceiveRecord(std::string callsign, uint64_t frequency, char snr);
    virtual void send();

private:
    unsigned int currentSequenceNumber_;
    unsigned int randomIdentifier_;
    
    std::string receiverCallsign_;
    std::string receiverGridSquare_;
    std::string decodingSoftware_;
    std::vector<SenderRecord> recordList_;
    std::mutex recordListMutex_;
    
    int getRxDataSize_();    
    int getTxDataSize_();    
    void encodeReceiverRecord_(char* buf);    
    void encodeSenderRecords_(char* buf);
    
    bool reportCommon_();
};


#endif // PSK_REPORTER_H