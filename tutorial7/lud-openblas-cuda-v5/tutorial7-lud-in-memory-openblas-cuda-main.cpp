// NIST-developed software is provided by NIST as a public service. You may use, copy and distribute copies of the software in any medium, provided that you keep intact this entire notice. You may improve, modify and create derivative works of the software or any portion of the software, and you may copy and distribute such modifications or works. Modified works should carry a notice stating that you changed the software and should note the date and nature of any such change. Please explicitly acknowledge the National Institute of Standards and Technology as the source of the software.
// NIST-developed software is expressly provided "AS IS." NIST MAKES NO WARRANTY OF ANY KIND, EXPRESS, IMPLIED, IN FACT OR ARISING BY OPERATION OF LAW, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, NON-INFRINGEMENT AND DATA ACCURACY. NIST NEITHER REPRESENTS NOR WARRANTS THAT THE OPERATION OF THE SOFTWARE WILL BE UNINTERRUPTED OR ERROR-FREE, OR THAT ANY DEFECTS WILL BE CORRECTED. NIST DOES NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE OF THE SOFTWARE OR THE RESULTS THEREOF, INCLUDING BUT NOT LIMITED TO THE CORRECTNESS, ACCURACY, RELIABILITY, OR USEFULNESS OF THE SOFTWARE.
// You are solely responsible for determining the appropriateness of using and distributing the software and you assume all risks associated with its use, including but not limited to the risks and costs of program errors, compliance with applicable laws, damage to or loss of data, programs or equipment, and the unavailability or interruption of operation. This software is not intended to be used in any situation where a failure could cause risk of injury or damage to property. The software developed by NIST employees is not subject to copyright protection within the United States.

//
// Created by tjb3 on 2/23/16.
//
//#define DEBUG_FLAG
//#define DEBUG_LEVEL_VERBOSE
//#define PROFILE

//typedef long long int lapack_int;

#include <htgs/api/TaskGraph.hpp>
#include <htgs/api/Runtime.hpp>
#include <cblas.h>
#include <iomanip>
#include <cfloat>
#include <magma_v2.h>

#include "data/MatrixRequestData.h"
#include "data/MatrixBlockData.h"
#include "rules/GausElimRuleUpper.h"
#include "rules/GausElimRuleLower.h"
#include "rules/GausElimRule.h"

#include "../../tutorial-utils/SimpleClock.h"
#include "tasks/CopyInPanelTask.h"
#include "tasks/GausElimTask.h"
#include "tasks/FactorLowerTask.h"
#include "tasks/CopyInPanelWindowTask.h"
#include "rules/MatrixMulRule.h"
#include "tasks/MatrixMulPanelTask.h"
#include "rules/GatherBlockRule.h"
#include "rules/UpdateFactorRule.h"
#include "tasks/CopyOutPanelTask.h"
#include "../../tutorial-utils/util-cuda.h"
#include "memory/CudaMatrixAllocator.h"
#include "rules/GausElimRuleUpperWindow.h"

int validateResults(double *luMatrix, double *origMatrix, int matrixSize) {
  int count = 0;

  // Multiply lower triangle with upper triangle

  double *lMatrix = new double[matrixSize * matrixSize];
  double *uMatrix = new double[matrixSize * matrixSize];
  double *result = new double[matrixSize*matrixSize];

  for (int c = 0; c < matrixSize; c++)
  {
    for (int r = 0; r < matrixSize; r++)
    {
      // below diag
      if (r > c)
      {
        lMatrix[IDX2C(r, c, matrixSize)] = luMatrix[IDX2C(r, c, matrixSize)];
        uMatrix[IDX2C(r, c, matrixSize)] = 0.0;
      }
        // above diag
      else if (c > r)
      {
        lMatrix[IDX2C(r, c, matrixSize)] = 0.0;
        uMatrix[IDX2C(r, c, matrixSize)] = luMatrix[IDX2C(r, c, matrixSize)];
      }
        // on diag
      else if (r == c)
      {
        lMatrix[IDX2C(r, c, matrixSize)] = 1.0;
        uMatrix[IDX2C(r, c, matrixSize)] = luMatrix[IDX2C(r, c, matrixSize)];
      }
    }
  }

  openblas_set_num_threads(40);
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, matrixSize, matrixSize, matrixSize, 1.0, lMatrix, matrixSize, uMatrix, matrixSize, 0.0, result, matrixSize);

  for (int r = 0; r < matrixSize; r++)
  {
    for (int c = 0; c < matrixSize; c++)
    {

      double difference = fabs(result[IDX2C(r, c, matrixSize)] - origMatrix[IDX2C(r, c, matrixSize)]);
      if (difference > 1.0e-8)
      {
        count++;
        if (count < 20)
        {
          std::cout << "Incorrect value: " << result[IDX2C(r, c, matrixSize)] << " != " << origMatrix[IDX2C(r, c, matrixSize)] << " difference: " << difference << std::endl;
        }
      }
    }
  }

  if (count > 0)
    std::cout << "Total incorrect = " << count << std::endl;

  delete [] lMatrix;
  delete [] uMatrix;
  delete [] result;

  if (count > 0)
    return 1;
  return 0;
}

void runSequentialLU(double *matrix, long long int matrixSize)
{
  magma_int_t *piv = new magma_int_t[matrixSize];
  magma_int_t info;
  magma_dgetrf(matrixSize, matrixSize, matrix, matrixSize, piv, &info);
}

int main(int argc, char *argv[]) {
  long matrixSize= 1000;
  int blockSize = 250;
  bool runSequential = false;
  bool validate = false;

  int numBlasThreads = 1;

  int numGausElimThreads = 1;
  int numFactorLowerThreads = 20;
  int numMatrixMulThreads = 1;
  int numPanelsFactor = 5;
  int numPanelsUpdate = 10;
  bool userDefinedUpdatePanels = false;
  int windowSize = 20;
  bool userDefinedWindowSize = false;
  int gpuId = 2;


  std::string runtimeFileStr("runtimes");

  int numRetry = 1;

  if (argc > 1) {
    for (int arg = 1; arg < argc; arg++) {
      std::string argvs(argv[arg]);

      if (argvs == "--size") {
        arg++;
        matrixSize = atoi(argv[arg]);
      }

      if (argvs == "--run-sequential") {
        runSequential = true;
      }

      if (argvs == "--update-mem") {
        arg++;
        numPanelsUpdate = atoi(argv[arg]);
        userDefinedUpdatePanels = true;
      }

      if (argvs == "--window-size") {
        arg++;
        windowSize = atoi(argv[arg]);
        userDefinedWindowSize = true;
      }

      if (argvs == "--gpu-id") {
        arg++;
        gpuId = atoi(argv[arg]);
      }


      if (argvs == "--factor-mem") {
        arg++;
        numPanelsFactor = atoi(argv[arg]);
      }

      if (argvs == "--num-threads-blas") {
        arg++;
        numBlasThreads = atoi(argv[arg]);
      }

      if (argvs == "--num-threads-factor-l") {
        arg++;
        numFactorLowerThreads = atoi(argv[arg]);
      }


      if (argvs == "--num-threads-gaus") {
        arg++;
        numGausElimThreads = atoi(argv[arg]);
      }

      if (argvs == "--num-threads-gemm") {
        arg++;
        numMatrixMulThreads = atoi(argv[arg]);
      }

      if (argvs == "--num-retry" && arg + 1 < argc) {
        arg++;
        numRetry = atoi(argv[arg]);
      }

      if (argvs == "--block-size") {
        arg++;
        blockSize = atoi(argv[arg]);
      }


      if (argvs == "--runtime-file" && arg + 1 < argc) {
        runtimeFileStr = argv[arg + 1];
        arg++;
      }

      if (argvs == "--validate-results") {
        validate = true;
      }

      if (argvs == "--help") {
        std::cout << argv[0]
                  << " args: [--size <#>] [--block-size <#>] [--num-retry <#>] [--factor-mem <#>] [--window-size <#>] [--runtime-file <filename>] [--validate-results] [--run-sequential] [--num-threads-factor-l <#>] [--num-threads-factor-u <#>] [--num-threads-gaus <#>] [--num-threads-gemm <#>] [--num-threads-blas <#>] [--help]"
                  << std::endl;
        exit(0);
      }
    }
  }

  std::ofstream runtimeFile(runtimeFileStr.c_str(), std::ios::app);

  double *matrix = new double[matrixSize * matrixSize];

  initMatrixDiagDom(matrix, matrixSize, matrixSize, true);

  double *matrixTest = nullptr;
  if (validate) {
    matrixTest = new double[matrixSize * matrixSize];

    for (int i = 0; i < matrixSize * matrixSize; i++)
      matrixTest[i] = matrix[i];
  }

  for (int numTry = 0; numTry < numRetry; numTry++) {
    SimpleClock clk;
    SimpleClock endToEnd;

    if (runSequential) {
      endToEnd.start();
      magma_init();
      magma_setdevice(gpuId);
      numBlasThreads = 20;
      openblas_set_num_threads(numBlasThreads);

      clk.start();
      runSequentialLU(matrix, matrixSize);
      clk.stopAndIncrement();
      endToEnd.stopAndIncrement();
    }
    else {
      endToEnd.start();
      openblas_set_num_threads(numBlasThreads);

      int gridHeight = (int) matrixSize / blockSize;
      int gridWidth = (int) matrixSize / blockSize;

      htgs::StateContainer<std::shared_ptr<MatrixBlockData<double *>>> *matrixBlocks = new htgs::StateContainer<std::shared_ptr<MatrixBlockData<double *>>>(gridHeight, gridWidth, nullptr);

      for (int r = 0; r < gridHeight; r++)
      {
        for (int c = 0; c < gridWidth; c++)
        {
          // Store pointer locations for all blocks
          double *ptr = &matrix[IDX2C(r * blockSize, c *blockSize, matrixSize)];

          std::shared_ptr<MatrixRequestData> request(new MatrixRequestData(r, c, MatrixType::MatrixA));
          std::shared_ptr<MatrixBlockData<double *>> data(new MatrixBlockData<double *>(request, ptr, blockSize, blockSize));

          matrixBlocks->set(r, c, data);
        }
      }

      int numGpus = 1;
      int *gpuIds = new int { gpuId };
      CUcontext * contexts = initCuda(numGpus, gpuIds);

      if (!userDefinedWindowSize) {
        size_t freeBytes = cudaGetFreeBytes(contexts[0]);
        // Compute size of one panel
        size_t panelSize = sizeof(double) * matrixSize * blockSize;

        size_t numPanelsInMemory = freeBytes / panelSize;

        // Check in core
        size_t numPanelsInCore = (size_t) matrixSize / blockSize;
        numPanelsInMemory -= numPanelsFactor;

        if (userDefinedUpdatePanels)
          numPanelsInMemory -= numPanelsUpdate;

        std::cout << "num panels in memory = " << numPanelsInMemory << " Num panels in core: " << numPanelsInCore
                  << std::endl;

        if (numPanelsInMemory >= numPanelsInCore + 1) {
          windowSize = (int) numPanelsInCore;
          if (!userDefinedUpdatePanels)
            numPanelsUpdate = 1;
        } else {
          // Current used memory for factor and update
//        size_t bytesUsed = (numPanelsFactor) * panelSize;
//        size_t panelMemoryAvailable = freeBytes - bytesUsed;

          int numPanelsAvailable = (int) numPanelsInMemory; //- 1;

          std::cout << "Num panels Available: " << numPanelsAvailable << std::endl;

          if (numPanelsAvailable <= 0) {
            std::cout << "Unable to fit in GPU in memory" << std::endl;
            exit(1);
          }

          switch (numPanelsAvailable) {
            case 0:std::cout << "Unable to fit in GPU in memory" << std::endl;
              exit(1);
            case 1:std::cout << "Unable to fit in GPU in memory" << std::endl;
              exit(1);
            case 2:windowSize = 1;
              if (!userDefinedUpdatePanels)
                numPanelsUpdate = 1;
              break;
            case 3:windowSize = 2;
              if (!userDefinedUpdatePanels)
                numPanelsUpdate = 1;
              break;
            default:
              if (userDefinedUpdatePanels) {
                windowSize = numPanelsAvailable;
              } else {
                if (numPanelsAvailable < 10) {

                  windowSize = numPanelsAvailable - 2;
                  if (!userDefinedUpdatePanels)
                    numPanelsUpdate = numPanelsAvailable - windowSize;
                } else {
                  windowSize = numPanelsAvailable - 5;
                  if (!userDefinedUpdatePanels)
                    numPanelsUpdate = numPanelsAvailable - windowSize;
                }
              }
          }
        }

      }

      std::cout << "Window = " << windowSize << " numUpdate = " << numPanelsUpdate << " numFactor = " << numPanelsFactor << " Num bytes available: " << cudaGetFreeBytes(contexts[0]) << std::endl;

      GausElimTask *gausElimTask = new GausElimTask(numGausElimThreads, matrixSize, matrixSize, blockSize);

      auto gausElimBk = new htgs::Bookkeeper<MatrixBlockData<double *>>();

      GausElimRuleUpperWindow *gausElimRuleUpperWindow = new GausElimRuleUpperWindow(matrixBlocks, gridHeight, gridWidth, blockSize, windowSize);
      GausElimRuleUpper *gausElimRuleUpper = new GausElimRuleUpper(matrixBlocks, gridHeight, gridWidth, blockSize, windowSize);
      GausElimRuleLower *gausElimRuleLower = new GausElimRuleLower(matrixBlocks, gridHeight, gridWidth);

      FactorLowerTask *factorLowerTask = new FactorLowerTask(numFactorLowerThreads, matrixSize, matrixSize);

      auto factorLowerBk = new htgs::Bookkeeper<MatrixBlockData<double *>>();
      auto gatherBlockRule = new GatherBlockRule(gridHeight, gridWidth, blockSize, matrixBlocks);

      CopyInPanelWindowTask *copyInUpperWindow = new CopyInPanelWindowTask(blockSize, contexts, gpuIds, numGpus, matrixSize, gridWidth, "UpdateWindowMem", PanelState::TOP_FACTORED);
      CopyInPanelTask *copyInUpper = new CopyInPanelTask(blockSize, contexts, gpuIds, numGpus, matrixSize, gridWidth, "UpdateMem", PanelState::TOP_FACTORED);
      CopyInPanelTask *copyInLower = new CopyInPanelTask(blockSize, contexts, gpuIds, numGpus, matrixSize, gridWidth, "FactorMem", PanelState::ALL_FACTORED);


      auto matrixMulBk = new htgs::Bookkeeper<MatrixPanelData>();
      MatrixMulRule *matrixMulRule = new MatrixMulRule(gridHeight, gridWidth);

      MatrixMulPanelTask *matrixMulTask = new MatrixMulPanelTask(contexts, gpuIds, numGpus, 1, matrixSize, matrixSize, matrixSize, matrixSize, blockSize);


      CopyOutPanelTask *copyResultBack = new CopyOutPanelTask(blockSize, contexts, gpuIds, numGpus, matrixSize, gridWidth, "FactorMem");

      auto matrixMulResultBk = new htgs::Bookkeeper<MatrixPanelData>();

      int numDiagonals = gridWidth - 1;
      GausElimRule *gausElimRule = new GausElimRule(numDiagonals, gridHeight, gridWidth, matrixBlocks);

      // Number of updates excluding the diagonal and the top/left row/column
      int numUpdates = (1.0/2.0) * (double)gridWidth * (gridWidth-1);
      UpdateFactorRule *updateFactorRule = new UpdateFactorRule(numUpdates, gridHeight, matrixBlocks);

      auto taskGraph = new htgs::TaskGraph<MatrixBlockData<double *>, htgs::VoidData>();
      taskGraph->addGraphInputConsumer(gausElimTask);

      taskGraph->addEdge(gausElimTask, gausElimBk);
      taskGraph->addRule(gausElimBk, copyInUpper, gausElimRuleUpper);
      taskGraph->addRule(gausElimBk, factorLowerTask, gausElimRuleLower);
      taskGraph->addRule(gausElimBk, copyInUpperWindow, gausElimRuleUpperWindow);

      taskGraph->addEdge(copyInUpper, matrixMulBk);
      taskGraph->addEdge(copyInUpperWindow, matrixMulBk);

      taskGraph->addEdge(factorLowerTask, factorLowerBk);

      taskGraph->addRule(factorLowerBk, copyInLower, gatherBlockRule);
      taskGraph->addEdge(copyInLower, matrixMulBk);

      taskGraph->addRule(matrixMulBk, matrixMulTask, matrixMulRule);
      taskGraph->addEdge(matrixMulTask, copyResultBack);
      taskGraph->addEdge(copyResultBack, matrixMulResultBk);

      if (numDiagonals > 0)
        taskGraph->addRule(matrixMulResultBk, gausElimTask, gausElimRule);
      else
        delete gausElimRule;

      if (numUpdates > 0)
        taskGraph->addRule(matrixMulResultBk, gausElimBk, updateFactorRule);
      else
        delete updateFactorRule;

      taskGraph->incrementGraphInputProducer();

      taskGraph->addCudaMemoryManagerEdge("FactorMem", copyInLower, matrixMulTask, new CudaMatrixAllocator(blockSize, matrixSize), numPanelsFactor, htgs::MMType::Static, contexts);
      taskGraph->addCudaMemoryManagerEdge("UpdateMem", copyInUpper, copyResultBack, new CudaMatrixAllocator(blockSize, matrixSize), numPanelsUpdate, htgs::MMType::Static, contexts);
      taskGraph->addCudaMemoryManagerEdge("UpdateWindowMem", copyInUpperWindow, copyResultBack, new CudaMatrixAllocator(blockSize, matrixSize), windowSize == 0 ? 1 : windowSize, htgs::MMType::Static, contexts);

//      taskGraph->writeDotToFile("lud-graph.dot");

      htgs::Runtime *runtime = new htgs::Runtime(taskGraph);

      clk.start();

      runtime->executeRuntime();


      taskGraph->produceData(matrixBlocks->get(0, 0));
      taskGraph->finishedProducingData();
      runtime->waitForRuntime();

      clk.stopAndIncrement();

      delete runtime;
      delete matrixBlocks;
      endToEnd.stopAndIncrement();
    }

    double operations = (2.0 * (matrixSize * matrixSize * matrixSize)) / 3.0;
    double flops = operations / clk.getAverageTime(TimeVal::SEC);
    double gflops = flops / 1073741824.0;




    std::cout << (runSequential ? "sequential" : "htgs")
              << ", matrix-size: " << matrixSize
              << ", " << "blockSize: " << (runSequential ? 0 : blockSize)
              << ", blasThreads: " << numBlasThreads
              << ", gausThreads: " << numGausElimThreads
              << ", factorLowerThreads: " << numFactorLowerThreads
              << ", gemmThreads: " << numMatrixMulThreads
              << ", time:" << clk.getAverageTime(TimeVal::MILLI)
              << ", end-to-end:" << endToEnd.getAverageTime(TimeVal::MILLI)
              << ", gflops: " << gflops
              << ", window: "<< windowSize
              << ", factor panels: " << numPanelsFactor
              << ", update panels: " << numPanelsUpdate
              << std::endl;

    runtimeFile << (runSequential ? "sequential" : "htgs")
                << ", " << matrixSize
                << ", " << blockSize
                << ", " << numBlasThreads
                << ", " << numGausElimThreads
                << ", " << numFactorLowerThreads
                << ", " << numMatrixMulThreads
                << ", " << clk.getAverageTime(TimeVal::MILLI)
                << ", " << endToEnd.getAverageTime(TimeVal::MILLI)
                << ", " << gflops
                << ", "<< windowSize
                << ", " << numPanelsFactor
                << ", " << numPanelsUpdate
                << std::endl;


    if (validate)
    {
      int res = validateResults(matrix, matrixTest, matrixSize);
      std::cout << (res == 0 ? "PASSED" : "FAILED") << std::endl;
    }
  }

  runtimeFile.close();
  delete[] matrix;

  if (validate)
    delete[] matrixTest;

}