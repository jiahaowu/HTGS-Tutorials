//
// Created by tjb3 on 2/23/16.
//

#include "../data/MatrixBlockMulData.h"
#include "../data/MatrixBlockData.h"
#ifndef HTGS_HADAMARDPRODUCTTASK_H
#define HTGS_HADAMARDPRODUCTTASK_H

class MatrixMulBlkTask : public htgs::ITask<MatrixBlockMulData, MatrixBlockData<double *>> {

 public:
  MatrixMulBlkTask(int numThreads) : ITask(numThreads) {}

  virtual ~MatrixMulBlkTask() {

  }
  virtual void initialize(int pipelineId,
                          int numPipeline) {

  }
  virtual void shutdown() {

  }
  virtual void executeTask(std::shared_ptr<MatrixBlockMulData> data) {

    auto matAData = data->getMatrixA();
    auto matBData = data->getMatrixB();

    double *matrixA = matAData->getMatrixData();
    double *matrixB = matBData->getMatrixData();

    int width = matAData->getMatrixWidth();
    int height = matAData->getMatrixHeight();

    double *result = new double[width * height];

    for (int i = 0; i < matAData->getMatrixWidth() * matAData->getMatrixHeight(); i++) {
      result[i] = matrixA[i] * matrixB[i];
    }

    auto matRequest = matAData->getRequest();

    std::shared_ptr<MatrixRequestData>
        matReq(new MatrixRequestData(matRequest->getRow(), matRequest->getCol(), MatrixType::MatrixC));

    addResult(new MatrixBlockData<double *>(matReq, result, width, height));

  }
  virtual std::string getName() {
    return "HadamardProductTask";
  }
  virtual htgs::ITask<MatrixBlockMulData, MatrixBlockData<double *>> *copy() {
    return new MatrixMulBlkTask(this->getNumThreads());
  }
  virtual bool isTerminated(std::shared_ptr<htgs::BaseConnector> inputConnector) {
    return inputConnector->isInputTerminated();
  }
};

#endif //HTGS_HADAMARDPRODUCTTASK_H
