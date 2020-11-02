#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>

#include "FFInputLayerJoin.h"
#include "FFMatrixBlock.h"
#include "FFMatrixMeta.h"
#include "FFMatrixData.h"
#include "FFMatrixBlockScanner.h"
#include "FFMatrixWriter.h"
#include "FFAggMatrix.h"
#include "FFReluBiasSum.h"
#include "FFTransposeBiasSum.h"
#include "FFTransposeMult.h"

#include "PDBClient.h"

using namespace std;

vector<vector<pair<int32_t, int32_t>>> labels;

vector<pair<int32_t, int32_t>> labels_meta;
vector<pair<int32_t, int32_t>> labels_data;

vector<vector<pair<int32_t, float>>> features;

int32_t total_points;

int32_t num_batch;
int32_t batch_block;

int32_t num_features;
int32_t features_block;

int32_t num_labels;
int32_t labels_block;

int32_t embedding_size;
int32_t embedding_block;

bool read_label(ifstream &is, char *buffer, int32_t batch_id) {

  int idx = 0;
  do {

    is.read(&buffer[idx], 1);
    if (buffer[idx] == ' ' || buffer[idx] == ',') {

      // add the end
      buffer[idx + 1] = '\0';

      // read the label
      char *tmp;
      auto label_id = strtol(buffer, &tmp, 10);

      // get the row id and col id
      auto row_id = batch_id / batch_block;
      auto col_id = label_id / labels_block;

      // store the labels
      labels[row_id * (num_labels / labels_block) + col_id].emplace_back(
          make_pair(batch_id % batch_block, label_id % labels_block));

      // does not have other labels
      return buffer[idx] == ',';
    }

    // increment the index
    idx++;

  } while (true);
}

void read_feature_idx(ifstream &is, char *buffer, int32_t pos) {

  int idx = 0;
  do {

    is.read(&buffer[idx], 1);
    if (buffer[idx] == ':') {

      // add the end
      buffer[idx + 1] = '\0';

      // read the label
      char *tmp;
      features[pos].emplace_back();
      features[pos].back().first = strtol(buffer, &tmp, 10);

      // does not have other labels
      return;
    }

    // increment the index
    idx++;

  } while (true);
}

bool read_feature_value(ifstream &is, char *buffer, int32_t pos) {

  int idx = 0;
  do {

    is.read(&buffer[idx], 1);
    if (buffer[idx] == ' ' || buffer[idx] == '\n') {

      // add the end
      buffer[idx + 1] = '\0';

      // read the label
      string::size_type tmp;
      features[pos].back().second = stof(buffer, &tmp);

      // does not have other labels
      return buffer[idx] == ' ';
    }

    // increment the index
    idx++;

  } while (true);
}

void init_features(int32_t row_id, int32_t col_id, double *values) {

  auto start_r = row_id * batch_block;
  auto end_r = (row_id + 1) * batch_block;

  for (int r = start_r; r < end_r; ++r) {

    // get the features row
    auto &features_row = features[r];

    // get the start and end column
    auto start_c = col_id * features_block;
    auto end_c = (col_id + 1) * features_block;

    for (const auto &f : features_row) {
      if (f.first >= start_c && f.first < end_c) {
        values[(r % batch_block) * features_block +
               (f.first % features_block)] = f.second;
      }
    }
  }
}

void send_features(pdb::PDBClient &pdbClient,
                   vector<pair<int32_t, int32_t>> &tuples_to_send,
                   string setName, string dbName) {
  string errMsg;
  size_t idx = 0;
  int sent = 0, totalRows = 0, totalCols = 0;
  while (idx != tuples_to_send.size()) {

    // use temporary allocation block
    const pdb::UseTemporaryAllocationBlock tempBlock{64 * 1024 * 1024};

    // put the chunks here
    pdb::Handle<pdb::Vector<pdb::Handle<FFMatrixBlock>>> vec =
        pdb::makeObject<pdb::Vector<pdb::Handle<FFMatrixBlock>>>();

    try {

      // put stuff into the vector
      for (; idx < tuples_to_send.size();) {

        // allocate a matrix
        pdb::Handle<FFMatrixBlock> myInt = pdb::makeObject<FFMatrixBlock>(
            tuples_to_send[idx].first, tuples_to_send[idx].second, batch_block,
            features_block);

        if (tuples_to_send[idx].first == 0)
          totalRows += embedding_block;
        if (tuples_to_send[idx].second == 0)
          totalCols += labels_block;

        // init the values
        double *vals = myInt->getValue().rawData->c_ptr();
        bzero(myInt->getValue().rawData->c_ptr(),
              batch_block * features_block * sizeof(double));

        init_features(tuples_to_send[idx].first, tuples_to_send[idx].second,
                      myInt->getValue().rawData->c_ptr());

        // we add the matrix to the block
        vec->push_back(myInt);

        // go to the next one
        ++idx;

        if (vec->size() == 50) {
          break;
        }
      }
    } catch (pdb::NotEnoughSpace &n) {
    }

    // init the records
    pdb::getRecord(vec);

    // send the data a bunch of times
    if (!pdbClient.sendData<FFMatrixBlock>(
            pair<string, string>(setName, dbName), vec, errMsg)) {
      cout << "Failed to send data to dispatcher server: " << errMsg << endl;
      exit(1);
    }

    sent += vec->size();
    // log that we stored stuff
    cout << "stored in input batch " << vec->size() << " !" << endl;
  }

  cout << "Sent input_batch("<< totalRows <<"x"<< totalCols <<") " << "sent " << sent << " blocks of input_batch of size " << batch_block << " x " << features_block << endl;
}

void load_input_data(pdb::PDBClient &pdbClient, string path, string dbName,
                     string setName) {
  if (path.size() == 0) {
    throw runtime_error("Invalid filepath for features: " + path);
  }

  /// 1. Load the data from the file

  // open the input file
  ifstream is(path);

  // load the data stats
  is >> total_points;
  is >> num_features;
  is >> num_labels;

  cout << total_points << ", " << num_features << ", " << num_labels << endl;

  // round features so we can pad them
  if ((num_features % features_block) != 0) {
    num_features += features_block - (num_features % features_block);
  }
  cout << "New num features: " << num_features << endl;

  // round labels so we can pad them
  if ((num_labels % labels_block) != 0) {
    num_labels += labels_block - (num_labels % labels_block);
  }
  cout << "New num labels: " << num_labels << endl;

  // check that we have enough data points
  if (total_points < num_batch) {
    throw runtime_error("Not enough data points to form a batch " +
                        to_string(total_points));
  }

  // init the data
  labels.resize((num_batch / batch_block) * (num_labels / labels_block));
  features.resize(num_batch);

  // load a single batch into memory
  char buffer[1024];
  for (int i = 0; i < num_batch; ++i) {

    // read the labels
    while (read_label(is, buffer, i)) {
    }

    do {

      // try to read the feature index
      read_feature_idx(is, buffer, i);

      // try to read the feature value
      if (!read_feature_value(is, buffer, i)) {
        break;
      }

    } while (true);
  }

  /// 2. Resize data on block size

  // figure out how many blocks we need to generate
  int32_t batch_s = num_batch / batch_block;
  int32_t batch_f = num_features / features_block;

  cout << "input_batch Blocks to generate : " << batch_s << " * " << batch_f << " = " << (batch_f * batch_s) << endl;

  // figure out all the blocks we need to send
  vector<pair<int32_t, int32_t>> tuples_to_send(batch_s * batch_f);
  for (int i = 0; i < batch_s; ++i) {
    for (int j = 0; j < batch_f; ++j) {
      tuples_to_send[i * batch_f + j] = {i, j};
    }
  }

  send_features(pdbClient, tuples_to_send, setName, dbName);

  int32_t batch_l = num_labels / labels_block;
  labels_meta.resize(batch_s * batch_l);
  cout << "labels Blocks to generate : " << batch_s << " * " << batch_l << " = " << (batch_l * batch_s) << endl;

  // figure out all the blocks we need to send
  for (int i = 0; i < batch_s; ++i) {
    for (int j = 0; j < batch_l; ++j) {

      //                                pos,                num
      labels_meta[i * batch_l + j] = {labels_data.size(),
                                      labels[i * batch_l + j].size()};

      // insert the labels data
      labels_data.insert(labels_data.end(), labels[i * batch_l + j].begin(),
                         labels[i * batch_l + j].end());
    }
  }
}

auto init_weights(pdb::PDBClient &pdbClient) {
  string errMsg;

  /// 1. Init the word embedding a matrix of size (num_features x
  /// embedding_block)

  auto block_f = num_features / features_block;
  auto block_e = embedding_size / embedding_block;

  // the page size
  const int32_t page_size = 64;

  cout << "W1 Blocks to generate : " << block_f << " * " << block_e << " = " << (block_f * block_e) << endl;

  // all the block we need to send
  std::vector<std::pair<int32_t, int32_t>> tuples_to_send(block_f * block_e);
  for (int i = 0; i < block_f; ++i) {
    for (int j = 0; j < block_e; ++j) {
      tuples_to_send[i * block_e + j] = {i, j};
    }
  }

  size_t idx = 0;
  int sent = 0, totalRows = 0, totalCols = 0;
  while (idx != tuples_to_send.size()) {

    // use temporary allocation block
    const pdb::UseTemporaryAllocationBlock tempBlock{page_size * 1024 * 1024};

    // put the chunks here
    pdb::Handle<pdb::Vector<pdb::Handle<FFMatrixBlock>>> data =
        pdb::makeObject<pdb::Vector<pdb::Handle<FFMatrixBlock>>>();

    try {

      // put stuff into the vector
      for (; idx < tuples_to_send.size();) {

        // allocate a matrix
        pdb::Handle<FFMatrixBlock> myInt = pdb::makeObject<FFMatrixBlock>(
            tuples_to_send[idx].first, tuples_to_send[idx].second,
            features_block, embedding_block);

        if (tuples_to_send[idx].first == 0)
          totalRows += features_block;
        if (tuples_to_send[idx].second == 0)
          totalCols += embedding_block;

        // init the values
        double *vals = myInt->getValue().rawData->c_ptr();
        for (int v = 0; v < features_block * embedding_block; ++v) {
          vals[v] = (double)drand48() * 0.1;
        }

        // check if we need to init the bias here if so do it...
        if (tuples_to_send[idx].first == (block_f - 1)) {

          // allocate the bias if necessary
          myInt->getValue().bias = pdb::makeObject<pdb::Vector<double>>(
              embedding_block, embedding_block);

          // init the bias
          for (int i = 0; i < embedding_block; ++i) {
            myInt->getValue().bias->c_ptr()[i] = (double)drand48() * 0.1;
          }
        }

        // we add the matrix to the block
        data->push_back(myInt);

        // go to the next one
        ++idx;

        if (data->size() == 50) {
          break;
        }
      }
    } catch (pdb::NotEnoughSpace &n) {
    }

    // init the records
    pdb::getRecord(data);

    // send the data a bunch of times
    // pdbClient.sendData<FFMatrixBlock>("ff", "w1", data);
    if (!pdbClient.sendData<FFMatrixBlock>(pair<string, string>("w1", "ff"),
                                           data, errMsg)) {
      cout << "Failed to send data to dispatcher server: " << errMsg << endl;
      exit(1);
    }

    sent += data->size();
    // log that we stored stuff
    std::cout << "stored in embedding " << data->size() << " !\n";
  }

  cout << "Sent W1("<< totalRows <<"x"<< totalCols <<") " << sent << " blocks of W1 with size " << features_block << " * " << embedding_block << " each" << endl;

  sent = 0;

  /// 2. Init the dense layer (embedding_block x block_l)

  // how many blocks we split the labels
  auto block_l = num_labels / labels_block;

  tuples_to_send.resize(block_e * block_l);
  for (int i = 0; i < block_e; ++i) {
    for (int j = 0; j < block_l; ++j) {
      tuples_to_send[i * block_l + j] = {i, j};
    }
  }
  cout << "W2 Blocks to generate : " << block_e << " * " << block_l << " = " << (block_e * block_l) << endl;

  idx = 0;
  totalRows = 0; totalCols = 0;
  while (idx != tuples_to_send.size()) {

    // use temporary allocation block
    const pdb::UseTemporaryAllocationBlock tempBlock{page_size * 1024 * 1024};

    // put the chunks here
    pdb::Handle<pdb::Vector<pdb::Handle<FFMatrixBlock>>> data =
        pdb::makeObject<pdb::Vector<pdb::Handle<FFMatrixBlock>>>();

    try {

      // put stuff into the vector
      for (; idx < tuples_to_send.size();) {

        // allocate a matrix
        pdb::Handle<FFMatrixBlock> myInt = pdb::makeObject<FFMatrixBlock>(
            tuples_to_send[idx].first, tuples_to_send[idx].second,
            embedding_block, labels_block);

        if (tuples_to_send[idx].first == 0)
          totalRows += embedding_block;
        if (tuples_to_send[idx].second == 0)
          totalCols += labels_block;

        // init the values
        double *vals = myInt->getValue().rawData->c_ptr();
        for (int v = 0; v < embedding_block * labels_block; ++v) {
          vals[v] = (double)drand48() * 0.1;
        }

        if (tuples_to_send[idx].first == (block_e - 1)) {

          // init the bias if necessary
          myInt->getValue().bias =
              makeObject<pdb::Vector<double>>(labels_block, labels_block);

          // init the bias
          for (int i = 0; i < labels_block; ++i) {
            myInt->getValue().bias->c_ptr()[i] = (double)drand48() * 0.1;
          }
        }

        // we add the matrix to the block
        data->push_back(myInt);

        // go to the next one
        ++idx;

        if (data->size() == 50) {
          break;
        }
      }
    } catch (pdb::NotEnoughSpace &n) {
    }

    // init the records
    pdb::getRecord(data);

    // send the data a bunch of times
    // pdbClient.sendData<ff::MatrixBlock>("ff", "w2", data);
    if (!pdbClient.sendData<FFMatrixBlock>(pair<string, string>("w2", "ff"),
                                           data, errMsg)) {
      cout << "Failed to send data to dispatcher server: " << errMsg << endl;
      exit(1);
    }

    sent += data->size();
    // log that we stored stuff
    std::cout << "stored in dense " << data->size() << " !\n";
  }

  cout << "Sent W2("<< totalRows <<"x"<< totalCols <<") " << sent << " blocks of W2 with size " << embedding_block << " * " << labels_block << " each" << endl;

}

void print(pdb::PDBClient &pdbClient, string dbName, string setName) {
  auto it = pdbClient.getSetIterator<FFMatrixBlock>(dbName, setName);

  for (auto r : it) {
    double *data = r->getRawDataHandle()->c_ptr();
    for (int i = 0; i < r->getRowNums() * r->getColNums(); i++) {
      std::cout << data[i] << ",";
    } 
  }
}

void print_stats(pdb::PDBClient &pdbClient, string dbName, string setName) {
  int rows = 0, cols = 0, blocks = 0;
  int totalRows = 0, totalCols = 0;
  int blockRows = 0, blockCols = 0;
  auto it = pdbClient.getSetIterator<FFMatrixBlock>(dbName, setName);

  for (auto r : it) {
    std::cout << r->getBlockRowIndex() << "," << r->getBlockColIndex() << ";";
    rows = r->getRowNums();
    cols = r->getColNums();
    if (r->getBlockRowIndex() == 0) {
      totalRows += r->getRowNums();
      blockRows += 1;
    }
    if (r->getBlockColIndex() == 0) {
      totalCols += r->getColNums();
      blockCols += 1;
    }
    blocks++;
  }

  std::cout << "\n" << setName << " (" << blockRows << " X " << blockCols << ") (" << blocks << ") : (" << totalRows << " x " << totalCols << "), Each block size: " << rows << " x " << cols << std::endl;
}

void loadMatrix (pdb::PDBClient & pdbClient, String dbName, String setName, int totalX, int totalY, int blockX, int blockY, std::string &errMsg) {

    int total = 0;
    pdb::makeObjectAllocatorBlock(128 * 1024 * 1024, true);

    pdb::Handle<pdb::Vector<pdb::Handle<FFMatrixBlock>>> storeMatrix1 =
        pdb::makeObject<pdb::Vector<pdb::Handle<FFMatrixBlock>>>();

    int numXBlocks = ceil(totalX / (double)blockX);
    int numYBlocks = ceil(totalY / (double)blockY);

    try {
        for (int i=0; i < numXBlocks; i++) {
            for (int j=0; j < numYBlocks; j++) {
                pdb::Handle<FFMatrixBlock> myData =
                    pdb::makeObject<FFMatrixBlock>(i, j, blockX, blockY);

                for (int ii = 0; ii < blockX; ii++) {
                    for (int jj = 0; jj < blockY; jj++) {
                        // row = i * blockX + ii, col = j * blockY + jj
                        double data = (i * blockX + ii) >= totalX || (j * blockY + jj) >= totalY ? 0 : i + j + ii + jj;
                        (*(myData->getRawDataHandle()))[ii * blockY + jj] = data;
                    }
                }

                // std::cout << "New block: " << total << std::endl;
                storeMatrix1->push_back(myData);
                total++;
            }
        }
        if (!pdbClient.sendData<FFMatrixBlock>(
            std::pair<std::string, std::string>(setName, dbName),
            storeMatrix1,
            errMsg)) {
                std::cout << "Failed to send data to dispatcher server" << std::endl;
                exit(1);
        }
    } catch (NotEnoughSpace& e) {
        std::cout << "Failed to send data to dispatcher server" << std::endl;
        exit(1);
    }
    std::cout << setName << "(" << totalX << "x" << totalY << "): (" << numXBlocks << " x " << numYBlocks << ")" << total << " blocks = " << blockX << " x " << blockY << " each" << std::endl;

   // to write back all buffered records
    pdbClient.flushData(errMsg);
}

void loadLibrary(pdb::PDBClient &pdbClient, string path) {
  string errMsg;
  if (!pdbClient.registerType(path, errMsg)) {
    cout << "Couldnt include " << path << ": " << errMsg << endl;
    exit(-1);
  }
}

void createSet(pdb::PDBClient &pdbClient, string dbName, string setName, string setName1) {
  string errMsg;
  if (!pdbClient.createSet<FFMatrixBlock>(
          dbName, setName, errMsg, (size_t)64 * (size_t)1024 * (size_t)1024,
          setName1)) {
    cout << "Not able to create set: " + errMsg;
    exit(-1);
  } else {
    cout << "Created set.\n";
  }
}

void createDatabase(pdb::PDBClient &pdbClient, string dbName) {
  string errMsg;
  if (!pdbClient.createDatabase(dbName, errMsg)) {
    cout << "Not able to create database: " << errMsg << endl;
    exit(-1);
  } else {
    cout << "Created database" << endl;
  }
}

int main(int argc, char *argv[]) {
  string errMsg;

  // cout << "num_batch (number of features to be picked from input file) : " << endl;
  // cin >> num_batch;

  // cout << "batch_block : " << endl;
  // cin >> batch_block;

  // cout << "features_block : " << endl;
  // cin >> features_block;

  // cout << "labels_block : " << endl;
  // cin >> labels_block;

  // cout << "embedding_size : " << endl;
  // cin >> embedding_size;

  // cout << "embedding_block : " << endl;
  // cin >> embedding_block;

  // string path;
  // cout << "Path to input: " << endl;
  // cin >> path;

  string masterIp = "localhost";
  pdb::PDBLoggerPtr clientLogger = make_shared<pdb::PDBLogger>("FFclientLog");
  pdb::PDBClient pdbClient(8108, masterIp, clientLogger, false, true);
  pdb::CatalogClient catalogClient(8108, masterIp, clientLogger);

  loadLibrary(pdbClient, "libraries/libFFMatrixMeta.so");
  loadLibrary(pdbClient, "libraries/libFFMatrixData.so");
  loadLibrary(pdbClient, "libraries/libFFMatrixBlock.so");
  loadLibrary(pdbClient, "libraries/libFFMatrixBlockScanner.so");
  loadLibrary(pdbClient, "libraries/libFFInputLayerJoin.so");
  loadLibrary(pdbClient, "libraries/libFFMatrixWriter.so");
  loadLibrary(pdbClient, "libraries/libFFAggMatrix.so");
  loadLibrary(pdbClient, "libraries/libFFReluBiasSum.so");
  loadLibrary(pdbClient, "libraries/libFFTransposeMult.so");
  loadLibrary(pdbClient, "libraries/libFFTransposeBiasSum.so");

  createDatabase(pdbClient, "ff");

  createSet(pdbClient, "ff", "inputs", "inputs");
  createSet(pdbClient, "ff", "label", "label");

  createSet(pdbClient, "ff", "w1", "W1");
  createSet(pdbClient, "ff", "b1", "B1");
  createSet(pdbClient, "ff", "temp_y1", "TempY1");
  createSet(pdbClient, "ff", "y1", "Y1");

  createSet(pdbClient, "ff", "w2", "W2");
  createSet(pdbClient, "ff", "b2", "B2");
  createSet(pdbClient, "ff", "temp_y2", "TempY2");
  createSet(pdbClient, "ff", "y2", "Y2");

  createSet(pdbClient, "ff", "wo", "WO");
  createSet(pdbClient, "ff", "bo", "BO");
  createSet(pdbClient, "ff", "temp_yo", "TempYO");
  createSet(pdbClient, "ff", "yo", "YO");

  // load the input data
  // load_input_data(pdbClient, path, "ff", "input_batch");
  // init_weights(pdbClient);

  int batch_size = 400;
  int features = 5000;
  int hid1_size = 128;
  int hid2_size = 256;
  int num_labels = 2;
  int block_x = 20;
  int block_y = 20;

  // X x features_size = None x 5000
  loadMatrix(pdbClient, "ff", "inputs", batch_size, features, block_x, block_y, errMsg);
  // X x labels_size = ???
  // loadMatrix(pdbClient, "ff", "label", 64, null, 1, 100, 2, errMsg);

  // 128 x features_size = 128 x 5000
  loadMatrix(pdbClient, "ff", "w1", hid1_size, features, block_x, block_y, errMsg);
  // 128 x 1
  loadMatrix(pdbClient, "ff", "b1", hid1_size, batch_size, block_x, block_y, errMsg);

  // 256 x 128
  loadMatrix(pdbClient, "ff", "w2", hid2_size, hid1_size, block_x, block_y, errMsg);
  // 256 x 1
  loadMatrix(pdbClient, "ff", "b2", hid2_size, batch_size, block_x, block_y, errMsg);

  // 2 x 256
  loadMatrix(pdbClient, "ff", "wo", num_labels, hid2_size, block_x, block_y, errMsg);
  // 2 x 1
  loadMatrix(pdbClient, "ff", "bo", num_labels, batch_size, block_x, block_y, errMsg);


  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "w1");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "inputs");

    pdb::Handle<pdb::Computation> join = pdb::makeObject<FFTransposeMult>();
    join->setInput(0, readA);
    join->setInput(1, readB);

    // make the aggregation
    pdb::Handle<pdb::Computation> myAggregation = pdb::makeObject<FFAggMatrix>();
    myAggregation->setInput(join);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "temp_y1");
    myWriter->setInput(myAggregation);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "temp_y1");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "b1");

    pdb::Handle<pdb::Computation> reluBias = pdb::makeObject<FFReluBiasSum>();
    reluBias->setInput(0, readA);
    reluBias->setInput(1, readB);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "y1");
    myWriter->setInput(reluBias);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    print_stats(pdbClient, "ff", "w1");
    print_stats(pdbClient, "ff", "inputs");
    print_stats(pdbClient, "ff", "b1");
    print_stats(pdbClient, "ff", "temp_y1");
    print_stats(pdbClient, "ff", "y1");
  }

  pdbClient.deleteSet("ff", "temp_y1");

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "w2");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "y1");

    pdb::Handle<pdb::Computation> join = pdb::makeObject<FFInputLayerJoin>();
    join->setInput(0, readA);
    join->setInput(1, readB);

    // make the aggregation
    pdb::Handle<pdb::Computation> myAggregation = pdb::makeObject<FFAggMatrix>();
    myAggregation->setInput(join);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "temp_y2");
    myWriter->setInput(myAggregation);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "temp_y2");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "b2");

    pdb::Handle<pdb::Computation> reluBias = pdb::makeObject<FFReluBiasSum>();
    reluBias->setInput(0, readA);
    reluBias->setInput(1, readB);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "y2");
    myWriter->setInput(reluBias);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    print_stats(pdbClient, "ff", "w2");
    print_stats(pdbClient, "ff", "y1");
    print_stats(pdbClient, "ff", "b2");
    print_stats(pdbClient, "ff", "temp_y2");
    print_stats(pdbClient, "ff", "y2");
  }

  pdbClient.deleteSet("ff", "temp_y2");

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "wo");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "y2");

    pdb::Handle<pdb::Computation> join = pdb::makeObject<FFInputLayerJoin>();
    join->setInput(0, readA);
    join->setInput(1, readB);

    // make the aggregation
    pdb::Handle<pdb::Computation> myAggregation = pdb::makeObject<FFAggMatrix>();
    myAggregation->setInput(join);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "temp_yo");
    myWriter->setInput(myAggregation);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    // make the computation
    pdb::Handle<pdb::Computation> readA =
        makeObject<FFMatrixBlockScanner>("ff", "temp_yo");
    pdb::Handle<pdb::Computation> readB =
        makeObject<FFMatrixBlockScanner>("ff", "bo");

    pdb::Handle<pdb::Computation> reluBias = pdb::makeObject<FFTransposeBiasSum>();
    reluBias->setInput(0, readA);
    reluBias->setInput(1, readB);

    // make the writer
    pdb::Handle<pdb::Computation> myWriter = pdb::makeObject<FFMatrixWriter>("ff", "yo");
    myWriter->setInput(reluBias);

    // run the computation
    if (!pdbClient.executeComputations(errMsg, myWriter)) {
      std::cout << "Computation failed. Message was: " << errMsg << "\n";
      return 1;
    }
  }

  {
    const pdb::UseTemporaryAllocationBlock tempBlock{1024 * 1024 * 128};

    print_stats(pdbClient, "ff", "wo");
    print_stats(pdbClient, "ff", "y2");
    print_stats(pdbClient, "ff", "bo");
    print_stats(pdbClient, "ff", "temp_yo");
    print_stats(pdbClient, "ff", "yo");

    // print(pdbClient, "ff", "yo");
  }

  pdbClient.deleteSet("ff", "temp_yo");

  sleep(20);

  return 0;
}