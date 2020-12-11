#ifndef ZCOIN_BIP47CHANNEL_H
#define ZCOIN_BIP47CHANNEL_H

#include <string>

#include "serialize.h"
#include "streams.h"
#include "uint256.h"

#include "bip47/address.h"
#include "bip47/paymentcode.h"

class CWallet;

namespace bip47 {

static constexpr size_t AddressLookaheadNumber = 10;

class CPaymentChannel
{
public:
    enum struct Side {
        sender = 0,
        receiver
    };
public:
    CPaymentChannel(CPaymentCode const & theirPcode, CExtKey const & myChannelKey, Side side);

    typedef std::vector<CBitcoinAddress> AddrContT;

    CPaymentCode const & getTheirPcode() const;
    AddrContT generateTheirSecretAddresses(size_t fromAddr, size_t uptoAddr) const;

    CPaymentCode const & getMyPcode() const;
    AddrContT generateMySecretAddresses(size_t fromAddr, size_t uptoAddr) const;

    std::vector<unsigned char> getMaskedPayload(COutPoint const & outpoint, CKey const & outpointSecret) const;

    AddrContT const & generateMyUsedAddresses();
    AddrContT const & generateMyNextAddresses();
    bool markAddressUsed(CBitcoinAddress const &);

    std::string const & getLabel() const;
    void setLabel(std::string const & l);

private:
    CExtKey myChannelKey;
    CPaymentCode theirPcode;
    boost::optional<CPaymentCode> mutable myPcode;

    std::string label;
    size_t usedAddressCount;
    AddrContT usedAddresses;
    AddrContT nextAddresses;
    Side side;
};

}

#endif // ZCOIN_BIP47CHANNEL_H
