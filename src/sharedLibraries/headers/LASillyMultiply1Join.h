#ifndef SILLY_LA_MULTIPLY1_JOIN_H
#define SILLY_LA_MULTIPLY1_JOIN_H

// by Binhang, May 2017

#include "Lambda.h"
#include "LambdaCreationFunctions.h"
#include "JoinComp.h"
//#include "BuiltInMatrixBlock.h"
#include "MatrixBlock.h"

// LA libraries:
#include <eigen3/Eigen/Dense>

using namespace pdb;

class LASillyMultiply1Join : public JoinComp<MatrixBlock, MatrixBlock, MatrixBlock> {

public:
    ENABLE_DEEP_COPY

    LASillyMultiply1Join() {}

    Lambda<bool> getSelection(Handle<MatrixBlock> in1, Handle<MatrixBlock> in2) override {
        /*
        return makeLambda (in1, in2, [] (Handle<MatrixBlock> & in1, Handle<MatrixBlock> & in2) {
            return in1->getBlockColIndex() == in2->getBlockRowIndex();
        });
        */
        return makeLambdaFromMethod(in1, getBlockColIndex) ==
            makeLambdaFromMethod(in2,
                                 getBlockRowIndex);  // This can be recognized by the pdb optimizer.
    }


    Lambda<Handle<MatrixBlock>> getProjection(Handle<MatrixBlock> in1,
                                              Handle<MatrixBlock> in2) override {
        return makeLambda(in1, in2, [](Handle<MatrixBlock>& in1, Handle<MatrixBlock>& in2) {
            if (MatrixBlock::librayCode == EIGEN_CODE) {
                // std::cout <<"Test Eigen" << std::endl;
                // std::cout <<"Current Matrix1 :"<< std::endl;
                // in1->print();
                // std::cout <<"Current Matrix2 :"<< std::endl;
                // in2->print();
                if (in1->getColNums() != in2->getRowNums()) {
                    std::cerr << "Block dimemsions mismatch!" << std::endl;
                    exit(1);
                }
                int rowNums = in1->getRowNums();
                int colNums = in2->getColNums();
                int blockRowIndex = in1->getBlockRowIndex();
                int blockColIndex = in2->getBlockColIndex();
                int totalRows = in1->getTotalRowNums();
                int totalCols = in2->getTotalColNums();
                Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
                    currentMatrix1(
                        in1->getRawDataHandle()->c_ptr(), in1->getRowNums(), in1->getColNums());
                Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
                    currentMatrix2(
                        in2->getRawDataHandle()->c_ptr(), in2->getRowNums(), in2->getColNums());

                pdb::Handle<MatrixBlock> resultMatrixBlock = pdb::makeObject<MatrixBlock>(
                    blockRowIndex, blockColIndex, rowNums, colNums, totalRows, totalCols);
                Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
                    productMatrix(resultMatrixBlock->getRawDataHandle()->c_ptr(), rowNums, colNums);

                productMatrix = currentMatrix1 * currentMatrix2;

                // std::cout <<"Result Matrix :"<< std::endl;
                // resultMatrixBlock->print();

                return resultMatrixBlock;
            } else {
                std::cerr << "Wrong librayCode!" << std::endl;
                exit(1);
            }
        });
    }
};


#endif