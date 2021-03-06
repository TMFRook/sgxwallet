/*
    Copyright (C) 2020 SKALE Labs

    This file is part of skale-consensus.

    skale-consensus is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    skale-consensus is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with skale-consensus.  If not, see <https://www.gnu.org/licenses/>.

    @file ZMQMessage.cpp
    @author Stan Kladko
    @date 2020
*/

#include "common.h"
#include "sgxwallet_common.h"
#include <third_party/cryptlite/sha256.h>
#include <iostream>
#include <fstream>

#include "ZMQClient.h"
#include "SGXWalletServer.hpp"
#include "BLSSignReqMessage.h"
#include "BLSSignRspMessage.h"
#include "ECDSASignReqMessage.h"
#include "ECDSASignRspMessage.h"
#include "ZMQMessage.h"


uint64_t ZMQMessage::getUint64Rapid(const char *_name) {
    CHECK_STATE(_name);
    CHECK_STATE(d->HasMember(_name));
    const rapidjson::Value &a = (*d)[_name];
    CHECK_STATE(a.IsUint64());
    return a.GetUint64();
};

string ZMQMessage::getStringRapid(const char *_name) {
    CHECK_STATE(_name);
    CHECK_STATE(d->HasMember(_name));
    CHECK_STATE((*d)[_name].IsString());
    return (*d)[_name].GetString();
};




shared_ptr <ZMQMessage> ZMQMessage::parse(const char *_msg,
                                          size_t _size, bool _isRequest,
                                          bool _verifySig) {

    CHECK_STATE(_msg);
    CHECK_STATE2(_size > 5, ZMQ_INVALID_MESSAGE_SIZE);
    // CHECK NULL TERMINATED
    CHECK_STATE(_msg[_size] == 0);
    CHECK_STATE2(_msg[_size - 1] == '}', ZMQ_INVALID_MESSAGE);
    CHECK_STATE2(_msg[0] == '{', ZMQ_INVALID_MESSAGE);

    auto d = make_shared<rapidjson::Document>();

    d->Parse(_msg);

    CHECK_STATE2(!d->HasParseError(), ZMQ_COULD_NOT_PARSE);
    CHECK_STATE2(d->IsObject(), ZMQ_COULD_NOT_PARSE);

    CHECK_STATE2(d->HasMember("type"), ZMQ_NO_TYPE_IN_MESSAGE);
    CHECK_STATE2((*d)["type"].IsString(), ZMQ_NO_TYPE_IN_MESSAGE);
    string type = (*d)["type"].GetString();

    if (_verifySig) {
        CHECK_STATE2(d->HasMember("cert"),ZMQ_NO_CERT_IN_MESSAGE);
        CHECK_STATE2(d->HasMember("msgSig"), ZMQ_NO_SIG_IN_MESSAGE);
        CHECK_STATE2((*d)["cert"].IsString(), ZMQ_NO_CERT_IN_MESSAGE);
        CHECK_STATE2((*d)["msgSig"].IsString(), ZMQ_NO_SIG_IN_MESSAGE);

        auto cert = make_shared<string>((*d)["cert"].GetString());
        string hash = cryptlite::sha256::hash_hex(*cert);

        auto filepath = "/tmp/sgx_wallet_cert_hash_" + hash;

        std::ofstream outFile(filepath);

        outFile << *cert;

        outFile.close();

        static recursive_mutex m;

        EVP_PKEY *publicKey = nullptr;

        {
            lock_guard <recursive_mutex> lock(m);

            if (!verifiedCerts.exists(*cert)) {
                CHECK_STATE(SGXWalletServer::verifyCert(filepath));
                auto handles = ZMQClient::readPublicKeyFromCertStr(*cert);
                CHECK_STATE(handles.first);
                CHECK_STATE(handles.second);
                verifiedCerts.put(*cert, handles);
                remove(cert->c_str());
            }

            publicKey = verifiedCerts.get(*cert).first;

            CHECK_STATE(publicKey);

            auto msgSig = make_shared<string>((*d)["msgSig"].GetString());

            d->RemoveMember("msgSig");

            rapidjson::StringBuffer buffer;

            rapidjson::Writer<rapidjson::StringBuffer> w(buffer);

            d->Accept(w);

            auto msgToVerify = buffer.GetString();

            ZMQClient::verifySig(publicKey,msgToVerify, *msgSig );

        }
    }


    shared_ptr <ZMQMessage> result;

    if (_isRequest) {
        return buildRequest(type, d);
    } else {
        return buildResponse(type, d);
    }
}

shared_ptr <ZMQMessage> ZMQMessage::buildRequest(string &_type, shared_ptr <rapidjson::Document> _d) {
    if (_type == ZMQMessage::BLS_SIGN_REQ) {
        return make_shared<BLSSignReqMessage>(_d);
    } else if (_type == ZMQMessage::ECDSA_SIGN_REQ) {
        return
                make_shared<ECDSASignReqMessage>(_d);
    } else {
        BOOST_THROW_EXCEPTION(SGXException(-301, "Incorrect zmq message type: " +
                                                 string(_type)));
    }
}

shared_ptr <ZMQMessage> ZMQMessage::buildResponse(string &_type, shared_ptr <rapidjson::Document> _d) {
    if (_type == ZMQMessage::BLS_SIGN_RSP) {
        return
                make_shared<BLSSignRspMessage>(_d);
    } else if (_type == ZMQMessage::ECDSA_SIGN_RSP) {
        return
                make_shared<ECDSASignRspMessage>(_d);
    } else {
        BOOST_THROW_EXCEPTION(InvalidStateException("Incorrect zmq message request type: " + string(_type),
                                                    __CLASS_NAME__)
        );
    }
}

cache::lru_cache<string, pair < EVP_PKEY * , X509 *>>
ZMQMessage::verifiedCerts(256);