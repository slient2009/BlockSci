//
//  main.cpp
//  blocksci-test
//
//  Created by Harry Kalodner on 1/3/17.
//  Copyright © 2017 Harry Kalodner. All rights reserved.
//

#define BLOCKSCI_WITHOUT_SINGLETON

#include "file_writer.hpp"

#include <blocksci/chain/blockchain.hpp>
#include <blocksci/chain/block.hpp>

#include <internal/bitcoin_uint256_hex.hpp>
#include <internal/data_access.hpp>
#include <internal/mempool_index.hpp>

#include <bitcoinapi/bitcoinapi.h>

#include <clipp.h>

#include <nlohmann/json.hpp>

#include <range/v3/range_for.hpp>

#include <chrono>
#include <iostream>
#include <future>
#include <unordered_map>

#include <csignal>

using namespace blocksci;
using namespace std::chrono;

static volatile sig_atomic_t done = 0;

void term(int)
{
    done = 1;
}

int initializeRecordingFile(Blockchain &chain) {
    auto mempoolDir = chain.getAccess().config.mempoolDirectory();
    if (!mempoolDir.exists()){
        filesystem::create_directory(mempoolDir);
    }
    auto mostRecentBlock = chain[static_cast<int>(chain.size()) - 1];
    FixedSizeFileWriter<uint32_t> txIndexFile(chain.getAccess().config.mempoolDirectory()/"tx_index");
    auto fileNum = static_cast<int>(txIndexFile.size());
    txIndexFile.write(mostRecentBlock.endTxIndex());
    FixedSizeFileWriter<int32_t> blockIndexFile(chain.getAccess().config.mempoolDirectory()/"block_index");
    assert(static_cast<int>(blockIndexFile.size()) == fileNum);
    blockIndexFile.write(mostRecentBlock.height() + 1);
    return fileNum;
}

struct MempoolFiles {
    FixedSizeFileWriter<MempoolRecord> txTimeFile;
    FixedSizeFileWriter<BlockRecord> blockTimeFile;
    
    MempoolFiles(const filesystem::path &mempoolPath, int fileNum) :
    txTimeFile(mempoolPath/(std::to_string(fileNum) + "_tx")),
    blockTimeFile(mempoolPath/(std::to_string(fileNum) + "_block"))
    {}
};

class SaferBitcoinApi {
    std::string username;
    std::string password;
    std::string address;
    int port;
    BitcoinAPI bitcoinAPI;
    
public:
    
    SaferBitcoinApi(std::string username_, std::string password_, std::string address_, int port_) :
    username(std::move(username_)), password(std::move(password_)), address(std::move(address_)), port(port_), bitcoinAPI(username, password, address, port) {}
    
    std::vector<chaintip_t> getchaintips() {
        return bitcoinAPI.getchaintips();
    }
    
    int getblockcount() {
        return bitcoinAPI.getblockcount();
    }
    
    std::string getpreviousblockhash(const std::string& blockhash) {
        return bitcoinAPI.getpreviousblockhash(blockhash);
    }
    
    std::vector<std::string> getrawmempool() {
        return bitcoinAPI.getrawmempool();
    }
};

class MempoolRecorder {
    blocksci::Blockchain chain;
    blocksci::BlockHeight lastHeight;
    std::unordered_map<blocksci::uint256, MempoolRecord, std::hash<blocksci::uint256>> mempool;
    SaferBitcoinApi &bitcoinAPI;
    
    MempoolFiles files;
    std::unordered_map<std::string, std::pair<BlockRecord, int>> blocksSeen;
    
    static constexpr int heightCutoff = 1000;
public:
    MempoolRecorder(const std::string &dataLocation, SaferBitcoinApi &bitcoinAPI_) :
    chain(dataLocation),
    lastHeight(static_cast<int>(chain.size())),
    bitcoinAPI(bitcoinAPI_),
    files(chain.getAccess().config.mempoolDirectory(), initializeRecordingFile(chain)) {
        updateBlockTimes(0);
        updateTxTimes(1);
    }
    
    void updateBlockTimes(time_t time) {
        auto tips = bitcoinAPI.getchaintips();
        auto currentHeight = bitcoinAPI.getblockcount();
        for(auto &tip : tips) {
            std::string searchBlock = std::move(tip.hash);
            int height = tip.height;
            while (height >= std::max(currentHeight - heightCutoff, 0) &&
                   blocksSeen.insert(std::pair<std::string, std::pair<BlockRecord, int>>(searchBlock, {BlockRecord{time}, height})).second) {
                searchBlock = bitcoinAPI.getpreviousblockhash(searchBlock);
                height--;
            }
        }
    }
    
    void updateTxTimes(time_t time) {
        auto rawMempool = bitcoinAPI.getrawmempool();
        for (auto &txHashString : rawMempool) {
            auto txHash = uint256S(txHashString);
            auto it = mempool.find(txHash);
            if (it == mempool.end()) {
                mempool[txHash] = {time};
            }
        }
    }
    
    void updateMempool() {
        try {
            updateTxTimes(system_clock::to_time_t(system_clock::now()));
            updateBlockTimes(system_clock::to_time_t(system_clock::now()));
        } catch (BitcoinException& e){
            std::cerr << "Failed to update mempool with error: " << e.what() << std::endl;
        }
    }
    
    void recordMempool() {
        chain.reload();
        auto blockCount = static_cast<BlockHeight>(chain.size());
        for (; lastHeight < blockCount; lastHeight++) {
            auto block = chain[lastHeight];
            time_t time;
            auto blockIt = blocksSeen.find(block.getHash().GetHex());
            if (blockIt == blocksSeen.end()) {
                system_clock::time_point tp = system_clock::now();
                time = system_clock::to_time_t(tp);
            } else {
                time = blockIt->second.first.observationTime;
            }
            files.blockTimeFile.write({time});
            RANGES_FOR(auto tx, chain[lastHeight]) {
                auto it = mempool.find(tx.getHash());
                if (it != mempool.end()) {
                    auto &txData = it->second;
                    files.txTimeFile.write(txData);
                    mempool.erase(it);
                } else {
                    files.txTimeFile.write({0});
                }
            }
        }
        files.txTimeFile.flush();
        files.blockTimeFile.flush();
    }
    
    void clearOldMempool() {
        {
            system_clock::time_point tp = system_clock::now();
            tp -= std::chrono::hours(5 * 24);
            auto clearTime = system_clock::to_time_t(tp);
            auto it = mempool.begin();
            while (it != mempool.end()) {
                if (it->second.time < clearTime) {
                    it = mempool.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        {
            auto currentHeight = bitcoinAPI.getblockcount();
            auto it = blocksSeen.begin();
            while (it != blocksSeen.end()) {
                if (it->second.second < currentHeight - heightCutoff) {
                    it = blocksSeen.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};


int main(int argc, char * argv[]) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = term;
    sigaction(SIGTERM, &action, nullptr);
    
    std::string configFilePathString;
    auto cli = clipp::group{clipp::value("config file", configFilePathString) % "Path to config file"};
    
    auto res = parse(argc, argv, cli);
    if (res.any_error()) {
        std::cout << "Invalid command line parameter\n" << clipp::make_man_page(cli, argv[0]);
        return 0;
    }
    
    filesystem::path configFilePath = {configFilePathString};
    auto jsonConf = blocksci::loadConfig(configFilePath.str());
    blocksci::checkVersion(jsonConf);
    
    blocksci::ChainRPCConfiguration rpcConfig = jsonConf.at("parser").at("rpc");

    SaferBitcoinApi bitcoinAPI{rpcConfig.username, rpcConfig.password, rpcConfig.address, rpcConfig.port};
    
    auto connected = false;
    while (!connected) {
        try {
            bitcoinAPI.getrawmempool();
            connected = true;
        } catch (BitcoinException &) {
            std::cerr << "Mempool recorder failed to connect to bitcoin node with {username = " << rpcConfig.username
            << ", password = " << rpcConfig.password
            << ", address = " << rpcConfig.address
            << ", port = " << rpcConfig.port << "}" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }
    
    MempoolRecorder recorder{configFilePath.str(), bitcoinAPI};
    
    int updateCount = 0;
    while(!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        recorder.updateMempool();
        updateCount++;
        if (updateCount % 5 == 0) {
            recorder.recordMempool();
        }
        if (updateCount % (4 * 60 * 60 * 24) == 0) {
            recorder.clearOldMempool();
            updateCount = 0;
        }
    }
    
    std::cout << "Shut down mempool recorder\n";
}
