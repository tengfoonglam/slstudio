#include "SLScanWorker.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTest>
#include <QTime>

#include <iostream>

#include <opencv2/opencv.hpp>
#include "cvtools.h"

#include "CodecFastRatio.h"
#include "CodecGrayCode.h"
#include "CodecPhaseShift2p1.h"
#include "CodecPhaseShift2p1Tpu.h"
#include "CodecPhaseShift2x3.h"
#include "CodecPhaseShift3.h"
#include "CodecPhaseShift3FastWrap.h"
#include "CodecPhaseShift3Unwrap.h"
#include "CodecPhaseShift4.h"
#include "CodecPhaseShiftDescatter.h"
#include "CodecPhaseShiftMicro.h"
#include "CodecPhaseShiftModulated.h"
#include "CodecPhaseShiftNStep.h"

#include "ProjectorLC3000.h"
#include "ProjectorLC4500.h"
#include "ProjectorLC4500Versavis.h"
#include "ProjectorOpenGL.h"

#include "CameraSpinnaker.h"
#include "SLCameraVirtual.h"
#include "SLPointCloudWidget.h"
#include "SLProjectorVirtual.h"

#include <chrono>
#include <thread>

void SLScanWorker::setup() {
  QSettings settings("SLStudio");

  // Read trigger configuration
  QString sTriggerMode = settings.value("trigger/mode", "Hardware").toString();
  if (sTriggerMode == "hardware")
    triggerMode = triggerModeHardware;
  else if (sTriggerMode == "software")
    triggerMode = triggerModeSoftware;
  else
    std::cerr << "SLScanWorker: invalid trigger mode "
              << sTriggerMode.toStdString() << std::endl;

  // Create camera
  int iNum = settings.value("camera/interfaceNumber", -1).toInt();
  int cNum = settings.value("camera/cameraNumber", -1).toInt();
  if (iNum != -1)
    camera = Camera::NewCamera(iNum, cNum, triggerMode);
  else
    camera = new SLCameraVirtual(cNum, triggerMode);

  // Temp use point grey because trigger wire broke :(
  // camera = new CameraSpinnaker(0, triggerMode);

  // Set camera settings
  CameraSettings camSettings;
  camSettings.shutter = settings.value("camera/shutter", 16.666).toFloat();
  camSettings.gain = 0.0;
  camera->setCameraSettings(camSettings);

  // Initialize projector
  int screenNum = settings.value("projector/screenNumber", -1).toInt();
  if (screenNum >= 0)
    projector = new ProjectorOpenGL(screenNum);
  else if (screenNum == -1)
    projector = new SLProjectorVirtual(screenNum);
  else if (screenNum == -2)
    projector = new ProjectorLC3000(0);
  else if (screenNum == -3)
    projector = new ProjectorLC4500(0);
  else if (screenNum == -4)
    projector = new ProjectorLC4500Versavis(0);
  else
    std::cerr << "SLScanWorker: invalid projector id " << screenNum
              << std::endl;

  CodecDir dir_init =
      (CodecDir)settings.value("pattern/direction", CodecDirHorizontal).toInt();

  auto display_horizontal_pattern = std::make_shared<bool>(
      (dir_init == CodecDirHorizontal || dir_init == CodecDirBoth) ? true
                                                                   : false);

  auto display_vertical_pattern = std::make_shared<bool>(
      (dir_init == CodecDirVertical || dir_init == CodecDirBoth) ? true
                                                                 : false);

  // Initialize encoder
  bool diamondPattern =
      settings.value("projector/diamondPattern", false).toBool();
  QString patternMode =
      settings.value("pattern/mode", "CodecPhaseShift3").toString();

  unsigned int screenResX, screenResY;
  projector->getScreenRes(&screenResX, &screenResY);

  // Unique number of rows and columns
  unsigned int screenCols, screenRows;
  if (diamondPattern) {
    screenCols = 2 * screenResX;
    screenRows = screenResY;
  } else {
    screenCols = screenResX;
    screenRows = screenResY;
  }

  CodecDir dir =
      (CodecDir)settings.value("pattern/direction", CodecDirHorizontal).toInt();
  if (dir == CodecDirNone)
    std::cerr << "SLScanWorker: invalid coding direction " << std::endl;

  if (patternMode == "CodecPhaseShift3")
    encoder = new EncoderPhaseShift3(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift4")
    encoder = new EncoderPhaseShift4(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift2x3")
    encoder = new EncoderPhaseShift2x3(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift3Unwrap")
    encoder = new EncoderPhaseShift3Unwrap(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShiftNStep")
    encoder = new EncoderPhaseShiftNStep(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift3FastWrap")
    encoder = new EncoderPhaseShift3FastWrap(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift2p1")
    encoder = new EncoderPhaseShift2p1(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShift2p1Tpu")
    encoder = new EncoderPhaseShift2p1Tpu(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShiftDescatter")
    encoder = new EncoderPhaseShiftDescatter(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShiftModulated")
    encoder = new EncoderPhaseShiftModulated(screenCols, screenRows, dir);
  else if (patternMode == "CodecPhaseShiftMicro")
    encoder = new EncoderPhaseShiftMicro(screenCols, screenRows, dir);
  else if (patternMode == "CodecFastRatio")
    encoder = new EncoderFastRatio(screenCols, screenRows, dir);
  else if (patternMode == "CodecGrayCode")
    encoder = new EncoderGrayCode(screenCols, screenRows, dir);
  else
    std::cerr << "SLScanWorker: invalid pattern mode "
              << patternMode.toStdString() << std::endl;

  // Init projector
  auto is_hardware_triggered = std::make_shared<bool>(
      (triggerMode == triggerModeHardware) ? true : false);
  auto void_is_hardware_triggered =
      std::static_pointer_cast<void>(is_hardware_triggered);
  projector->loadParam("is_hardware_triggered", void_is_hardware_triggered);

  auto is_in_calibration_mode = std::make_shared<bool>(false);
  auto void_is_in_calibration_mode =
      std::static_pointer_cast<void>(is_in_calibration_mode);
  projector->loadParam("is_in_calibration_mode", void_is_in_calibration_mode);

  auto is_2_plus_1_mode = std::make_shared<bool>(
      (patternMode == "CodecPhaseShift2p1Tpu") ? true : false);
  auto void_is_2_plus_1_mode = std::static_pointer_cast<void>(is_2_plus_1_mode);
  projector->loadParam("is_2_plus_1_mode", void_is_2_plus_1_mode);

  auto void_display_vertical_pattern =
      std::static_pointer_cast<void>(display_vertical_pattern);
  projector->loadParam("display_vertical_pattern",
                       void_display_vertical_pattern);

  auto void_display_horizontal_pattern =
      std::static_pointer_cast<void>(display_horizontal_pattern);
  projector->loadParam("display_horizontal_pattern",
                       void_display_horizontal_pattern);

  projector->init();

  // Lens correction and upload patterns to projector/GPU
  CalibrationData calibration;
  calibration.load("calibration.xml");

  cv::Mat map1, map2;
  cv::Size mapSize = cv::Size(screenCols, screenRows);
  cvtools::initDistortMap(calibration.Kp, calibration.kp, mapSize, map1, map2);

  std::cout << calibration.Kp << std::endl;

  std::cout << calibration.kp << std::endl;

  // Upload patterns to projector/GPU in full projector resolution
  for (unsigned int i = 0; i < encoder->getNPatterns(); i++) {
    cv::Mat pattern = encoder->getEncodingPattern(i);

    // general repmat
    pattern = cv::repeat(pattern, screenRows / pattern.rows + 1,
                         screenCols / pattern.cols + 1);
    pattern = pattern(cv::Range(0, screenRows), cv::Range(0, screenCols));

    // correct for lens distortion
    cv::remap(pattern, pattern, map1, map2, CV_INTER_CUBIC);

    if (diamondPattern) pattern = cvtools::diamondDownsample(pattern);

    projector->setPattern(i, pattern.ptr(), pattern.cols, pattern.rows);

    cv::imwrite(cv::format("scan_pat_%d.bmp", i), pattern);
  }

  //    // Upload patterns to projector/GPU in compact resolution (texture)
  //    for(unsigned int i=0; i<encoder->getNPatterns(); i++){
  //        cv::Mat pattern = encoder->getEncodingPattern(i);
  //        if(diamondPattern){
  //            // general repmat
  //            pattern = cv::repeat(pattern, screenRows/pattern.rows+1,
  //            screenCols/pattern.cols+1); pattern = pattern(cv::Range(0,
  //            screenRows), cv::Range(0, screenCols)); pattern =
  //            cvtools::diamondDownsample(pattern);
  //        }
  //        projector->setPattern(i, pattern.ptr(), pattern.cols, pattern.rows);
  //    }

  // Read aquisition mode
  QString sAquisition = settings.value("aquisition").toString();
  if (sAquisition == "continuous")
    aquisition = aquisitionContinuous;
  else if (sAquisition == "single")
    aquisition = aquisitionSingle;
  else
    std::cerr << "SLScanWorker: invalid aquisition mode "
              << sAquisition.toStdString() << std::endl;

  writeToDisk = settings.value("writeToDisk/frames", false).toBool();
}

void SLScanWorker::doWork() {
  // State variable
  isWorking = true;

  unsigned long k = 0;

  std::cout << "Starting capture!" << std::endl;
  camera->startCapture();

  unsigned int N = encoder->getNPatterns();

  QSettings settings("SLStudio");
  unsigned int shift = settings.value("trigger/shift", "0").toInt();
  unsigned int delay = settings.value("trigger/delay", "100").toInt();

  QTime time;
  time.start();

  // Processing loop
  do {
    std::vector<cv::Mat> frameSeq(N);
    bool success = true;

    time.restart();

    projector->displayPattern(0);

    // Acquire patterns
    for (unsigned int i = 0; i < N; i++) {
      // Project coded pattern
      projector->displayPattern(i);

      if (triggerMode == triggerModeSoftware) {
        // Wait one frame period to rotate projector frame buffer
        QTest::qSleep(delay);
      } else {
        // Wait a few milliseconds to allow camera to get ready
        // QTest::qSleep(1);
      }

      if (i == 0 && triggerMode == triggerModeHardware) {
        // First pattern we delay entire camera by 1ms behind pattern changes
        // std::this_thread::sleep_for(std::chrono::milliseconds(1)); // For
        // blackfly

        // std::this_thread::sleep_for(std::chrono::milliseconds(15)); //
        // Grasshopper overlapping trigger

        std::this_thread::sleep_for(std::chrono::milliseconds(0));
      }

      if (triggerMode == triggerModeHardware) {
        camera->get_input("expected_image_time",
                          projector->getOutput("expected_image_time"));
      }

      CameraFrame frame;
      frame = camera->getFrame();

      if (!frame.memory) {
        std::cerr << "SLScanWorker: missed frame!" << std::endl;
        success = false;
      }

      std::cout << frame.flags << std::endl;

      // If the camera provides a sequence start flag
      if (frame.flags != 0) {
        std::cout << "reset" << std::endl;
        i = 0;
      }

      // Create 8 bit OpenCV matrix
      cv::Mat frameCV(frame.height, frame.width, CV_8U, frame.memory);
      frameCV = frameCV.clone();

      if (triggerMode == triggerModeHardware)
        frameSeq[(i + N - shift) % N] = frameCV;
      else
        frameSeq[i] = frameCV;
    }

    float sequenceTime = time.restart();
    cout << "Scan worker " << sequenceTime << "ms" << std::endl;

    //        // Check for missed frames
    //        if((triggerMode == triggerModeHardware) & (sequenceTime > N*18))
    //            success = false;

    if (!success) {
      std::cerr << "SLScanWorker: missed sequence!" << std::endl;
      continue;
    }

    // Write frames to disk if desired
    if (writeToDisk) {
      for (unsigned int i = 0; i < frameSeq.size(); i++) {
        QString filename = QString("frameSeq_%2_%1.bmp")
                               .arg(i, 2, 10, QChar('0'))
                               .arg(k, 2, 10, QChar('0'));
        filename = cv::imwrite(filename.toStdString(), frameSeq[i]);
      }
    }

    // Pass frame sequence to decoder
    emit newFrameSeq(frameSeq);

    // Calculate and show histogram of sumimage
    /**
    float range[] = {0, 255};
    const float* histRange = {range};
    int histSize = 256;
    cv::Mat histogram;
    cv::Mat frameSeqArr[] = {frameSeq[0], frameSeq[1], frameSeq[2]};
    const int channels[] = {0, 1, 2};
    cv::calcHist(frameSeqArr, 3, channels, cv::Mat(), histogram, 1, &histSize,
                 &histRange);
    // emit hist("Histogram", histogram, 100, 50);
    cv::Mat histogramImage = cvtools::histimage(histogram);
    emit showHistogram(histogramImage);
    **/

    // Increase iteration counter
    k += 1;

    // Process events to e.g. check for exit flag
    QCoreApplication::processEvents();

  } while (isWorking && (aquisition == aquisitionContinuous));

  // if (triggerMode == triggerModeHardware) camera->stopCapture();

  camera->stopCapture();

  projector->displayBlack();

  // Really dirty fix to close USB connection with Lightcrafter after a scan or
  // else there will be problems during the second scan by destroying projector
  // class object then creating a new one
  /**
  int screenNum = settings.value("projector/screenNumber", -1).toInt();
  if (screenNum == -4) {
    delete projector;
    projector = nullptr;

    delete camera;
    camera = nullptr;
  }
  **/

  // Emit message to e.g. initiate thread break down
  emit finished();
}

void SLScanWorker::stopWorking() { isWorking = false; }

SLScanWorker::~SLScanWorker() {
  if (camera != nullptr) {
    delete camera;
  }

  if (projector != nullptr) {
    delete projector;
  }
}
