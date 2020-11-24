#include "PDBClient.h"
#include "ReaderClient.h"
#include "RedditComment.h"

#include <string>
#include <queue>
#include <vector>

int main(int argc, char* argv[]){
    std::string errMsg = "Error occurred in loading Reddit Comments.";
    // make sure we have all the required arguments
    if(argc < 5) {
        std::cout << "Usage : ./LoadComments managerIP managerPort inputFileName whetherToPartitionData, whetherToRegisterLibraries\n";
        std::cout << "managerIP - IP of the manager\n";
        std::cout << "managerPort - Port of the manager\n";
        std::cout << "inputFileName - The file to load for reddit comments data, which is a set of JSON objects\n";
        std::cout << "whetherToPrepartitionData - Y yes, N no\n";
        std::cout << "whetherToRegisterLibraries - Y yes, N no\n";    
    }
    std::string managerIp = std::string(argv[1]);
    int32_t port = std::stoi(argv[2]);
    bool whetherToPartitionData = true;
    if (strcmp(argv[4], "N")==0) {
        whetherToPartitionData = false;
    }
    bool whetherToRegisterLibraries = true;
    if (strcmp(argv[5], "N")==0) {
        whetherToRegisterLibraries = false;
    }
    std::vector<std::string> pathVec;
    std::string dbName = "redditDB";
    std::string setName = "comments";
    // TODO: hard cooded paths
    std::string path_1 = "/home/ubuntu/data/2019_sample";
    std::string path_2 = "/home/ubuntu/data/2019_sample_2";
    std::string path_3 = "/home/ubuntu/data/2019_sample_3";
    pathVec.push_back(path_1);
    pathVec.push_back(path_2);
    pathVec.push_back(path_3);
    std::queue<std::ifstream *> commentsFiles;
    for(int i = 0; i < 3; i++){
        std::ifstream *filePtr = new std::ifstream(pathVec.at(i));
        commentsFiles.push(filePtr);
    }
    // Initialize all the required objects
    pdb::PDBLoggerPtr clientLogger =
        make_shared<pdb::PDBLogger>("RedditCommentsLogger");
    pdb::CatalogClient catalogClient(port, managerIp, clientLogger);
    pdb::PDBClient pdbClient(port, managerIp, clientLogger, false, true);
    pdbClient.registerType("libraries/libRedditComment.so", errMsg);
    // Lambda which can be used to identify a specific node for
    // dispatching the data at the time of loading.
    Handle<LambdaIdentifier> myLambda1 = nullptr;
    // now, create a new database
    pdbClient.createDatabase(dbName, errMsg);
    // now, create the output set
    pdbClient.removeSet(dbName, setName, errMsg);
    pdbClient.createSet<reddit::Comment>(dbName, setName, errMsg,
        (size_t)64*(size_t)1024*(size_t)1024, setName, nullptr, myLambda1);

    //Load the raw data into the output set
    ReaderClient rc(port, managerIp, clientLogger);
    rc.load<reddit::Comment>(1, commentsFiles, dbName, setName, 64,
    "libraries/libRedditComment.so", 20000000);
    return 0;
}