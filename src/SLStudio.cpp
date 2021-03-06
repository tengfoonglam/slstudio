#include "SLStudio.h"
#include "ui_SLStudio.h"

#include <stdio.h>
#include <string.h>
#include <QFileDialog>
#include <QThread>

#include "SLAboutDialog.h"
#include "SLCalibrationDialog.h"
#include "SLPreferenceDialog.h"

#include "SLVideoWidget.h"

#include <QSettings>
#include <QtGui>

#include "cvtools.h"

#include "CameraROS.h"
#include "CameraSpinnaker.h"
#include "Codec.h"
#include "CodecPhaseShift2p1Tpu.h"
#include "CodecPhaseShift2x3.h"
#include "ProjectorLC3000.h"
#include "ProjectorLC4500.h"
#include "ProjectorLC4500Versavis.h"
//#include "ProjectorOpenGL.h"

#include "SLCameraVirtual.h"
#include "SLProjectorVirtual.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace std;

SLStudio::SLStudio(QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::SLStudio),
      scanWorkerThread(NULL),
      settings(NULL),
      histogramDialog(NULL),
      shadingDialog(NULL),
      decoderUpDialog(NULL),
      decoderVpDialog(NULL),
      trackerDialog(NULL) {
  ui->setupUi(this);

  time = new QTime;

  // Restore main window geometry
  settings =
      new QSettings("SLStudio");  // This is stored here ~/.config/SLStudio.conf
  restoreGeometry(settings->value("geometry/mainwindow").toByteArray());
  restoreState(settings->value("state/mainwindow").toByteArray());

  // Ui connections
  connect(ui->pointCloudWidget, SIGNAL(newPointCloudDisplayed()), this,
          SLOT(updateDisplayRate()));

  // Create video dialogs
  histogramDialog = new SLVideoDialog("Histogram", this);
  shadingDialog = new SLVideoDialog("Shading", this);
  cameraFramesDialog = new SLVideoDialog("Camera Frames", this);
  decoderUpDialog = new SLVideoDialog("Decoder Up", this);
  decoderVpDialog = new SLVideoDialog("Decoder Vp", this);

  // Add view menu actions
  ui->menuView->addAction(histogramDialog->toggleViewAction());
  ui->menuView->addAction(shadingDialog->toggleViewAction());
  ui->menuView->addAction(cameraFramesDialog->toggleViewAction());
  ui->menuView->addAction(decoderUpDialog->toggleViewAction());
  ui->menuView->addAction(decoderVpDialog->toggleViewAction());

  // Restore Geometry
  histogramDialog->restoreGeometry(
      settings->value("geometry/histogram").toByteArray());
  shadingDialog->restoreGeometry(
      settings->value("geometry/shading").toByteArray());
  cameraFramesDialog->restoreGeometry(
      settings->value("geometry/cameraFrames").toByteArray());
  decoderUpDialog->restoreGeometry(
      settings->value("geometry/decoderUp").toByteArray());
  decoderVpDialog->restoreGeometry(
      settings->value("geometry/decoderVp").toByteArray());

  // Restore Visibility
  histogramDialog->setVisible(
      settings->value("visible/histogram", false).toBool());
  shadingDialog->setVisible(settings->value("visible/shading", false).toBool());
  cameraFramesDialog->setVisible(
      settings->value("visible/cameraFrames", false).toBool());
  decoderUpDialog->setVisible(
      settings->value("visible/decoderUp", false).toBool());
  decoderVpDialog->setVisible(
      settings->value("visible/decoderVp", false).toBool());

  // Tracker Dialog
  trackerDialog = new SLTrackerDialog(this);
  ui->menuView->addAction(trackerDialog->toggleViewAction());
  trackerDialog->setVisible(
      settings->value("visible/trackerDialog", false).toBool());
}

void SLStudio::onShowHistogram(cv::Mat im) {
  if (histogramDialog->isVisible()) histogramDialog->showImageCV(im);
}

void SLStudio::onShowShading(cv::Mat im) {
  if (shadingDialog->isVisible()) shadingDialog->showImageCV(im);
}

void SLStudio::onShowCameraFrames(std::vector<cv::Mat> frameSeq) {
  if (cameraFramesDialog->isVisible())
    cameraFramesDialog->showImageSeqCV(frameSeq);
}

void SLStudio::onShowDecoderUp(cv::Mat im) {
  if (decoderUpDialog->isVisible()) decoderUpDialog->showImageCV(im);
}

void SLStudio::onShowDecoderVp(cv::Mat im) {
  if (decoderVpDialog->isVisible()) {
    decoderVpDialog->showImageCV(im);
    // std::cout << "Showing now!" << std::endl;
  }
}

void SLStudio::onActionStart() {
  // Prepare scanWorker on separate thread
  scanWorker = new SLScanWorker(this);
  scanWorkerThread = new QThread(this);
  scanWorkerThread->setObjectName("scanWorkerThread");
  scanWorker->moveToThread(scanWorkerThread);
  connect(scanWorker, SIGNAL(finished()), this, SLOT(onScanWorkerFinished()));

  // Prepare decoderWorker on separate thread
  decoderWorker = new SLDecoderWorker();
  decoderThread = new QThread(this);
  decoderThread->setObjectName("decoderThread");
  decoderWorker->moveToThread(decoderThread);
  connect(decoderThread, SIGNAL(started()), decoderWorker, SLOT(setup()));
  connect(decoderThread, SIGNAL(finished()), decoderWorker,
          SLOT(deleteLater()));
  connect(decoderThread, SIGNAL(finished()), decoderThread,
          SLOT(deleteLater()));

  // Prepare triangulatorWorker on separate thread
  triangulatorWorker = new SLTriangulatorWorker();
  triangulatorThread = new QThread(this);
  triangulatorThread->setObjectName("triangulatorThread");
  triangulatorWorker->moveToThread(triangulatorThread);
  connect(triangulatorThread, SIGNAL(started()), triangulatorWorker,
          SLOT(setup()));
  connect(triangulatorThread, SIGNAL(finished()), triangulatorWorker,
          SLOT(deleteLater()));
  connect(triangulatorThread, SIGNAL(finished()), triangulatorThread,
          SLOT(deleteLater()));

  // Register metatypes
  qRegisterMetaType<cv::Mat>("cv::Mat");
  qRegisterMetaType<std::vector<cv::Mat> >("std::vector<cv::Mat>");
  qRegisterMetaType<PointCloudConstPtr>("PointCloudConstPtr");

  // Inter thread connections
  connect(scanWorker, SIGNAL(showHistogram(cv::Mat)), this,
          SLOT(onShowHistogram(cv::Mat)));
  connect(scanWorker, SIGNAL(newFrameSeq(std::vector<cv::Mat>)), decoderWorker,
          SLOT(decodeSequence(std::vector<cv::Mat>)));
  connect(scanWorker, SIGNAL(newFrameSeq(std::vector<cv::Mat>)), this,
          SLOT(onShowCameraFrames(std::vector<cv::Mat>)));
  connect(decoderWorker, SIGNAL(showShading(cv::Mat)), this,
          SLOT(onShowShading(cv::Mat)));
  connect(decoderWorker, SIGNAL(showDecoderUp(cv::Mat)), this,
          SLOT(onShowDecoderUp(cv::Mat)));
  connect(decoderWorker, SIGNAL(showDecoderVp(cv::Mat)), this,
          SLOT(onShowDecoderVp(cv::Mat)));
  connect(decoderWorker, SIGNAL(newUpVp(cv::Mat, cv::Mat, cv::Mat, cv::Mat)),
          triangulatorWorker,
          SLOT(triangulatePointCloud(cv::Mat, cv::Mat, cv::Mat, cv::Mat)));
  connect(triangulatorWorker, SIGNAL(newPointCloud(PointCloudConstPtr)), this,
          SLOT(receiveNewPointCloud(PointCloudConstPtr)));
  connect(triangulatorWorker, SIGNAL(imshow(const char *, cv::Mat, uint, uint)),
          this, SLOT(imshow(const char *, cv::Mat, uint, uint)));

  // Start threads
  decoderThread->start(QThread::LowPriority);
  triangulatorThread->start(QThread::LowPriority);
  scanWorkerThread->start(QThread::TimeCriticalPriority);

  // Setup and start processing
  QMetaObject::invokeMethod(decoderWorker, "setup");
  QMetaObject::invokeMethod(triangulatorWorker, "setup");
  QMetaObject::invokeMethod(scanWorker, "setup");
  QMetaObject::invokeMethod(scanWorker, "doWork");
  time->start();

  // Change ui elements
  ui->actionStart->setEnabled(false);
  ui->actionStop->setEnabled(true);
  ui->actionTracking->setEnabled(true);
  ui->actionSavePointCloud->setEnabled(true);
  ui->actionSaveScreenshot->setEnabled(true);
  ui->actionCalibration->setEnabled(false);
}

void SLStudio::onActionStop() {
  // Stop processing on scan worker thread
  QMetaObject::invokeMethod(scanWorker, "stopWorking");

  // cv::destroyAllWindows();

  decoderThread->quit();
  decoderThread->wait();

  std::cout << "decoderThread deleted\n" << std::flush;

  triangulatorThread->quit();
  triangulatorThread->wait();

  std::cout << "triangulatorThread deleted\n" << std::flush;
}

void SLStudio::onScanWorkerFinished() {
  QMetaObject::invokeMethod(scanWorker, "deleteLater");

  // Terminate scan worker thread
  scanWorkerThread->quit();
  scanWorkerThread->wait();
  scanWorkerThread->deleteLater();
  delete scanWorkerThread;

  // Change ui elements
  ui->actionStart->setEnabled(true);
  ui->actionStop->setEnabled(false);
  ui->actionTracking->setEnabled(false);
  ui->actionCalibration->setEnabled(true);
}

void SLStudio::onActionCalibration() {
  SLCalibrationDialog *calibrationDialog = new SLCalibrationDialog(this);
  calibrationDialog->exec();
}

void SLStudio::onActionPreferences() {
  SLPreferenceDialog *preferenceDialog = new SLPreferenceDialog(this);
  preferenceDialog->exec();
}

void SLStudio::updateDisplayRate() {
  int mSecElapsed = time->restart();
  displayIntervals.push_back(mSecElapsed);

  if (displayIntervals.size() > 10)
    displayIntervals.erase(displayIntervals.begin(),
                           displayIntervals.end() - 10);

  float meanMSecElapsed = 0;
  for (unsigned int i = 0; i < displayIntervals.size(); i++)
    meanMSecElapsed += displayIntervals[i];

  meanMSecElapsed /= displayIntervals.size();

  QString fpsString =
      QString("PCPS: %1").arg(1000.0 / meanMSecElapsed, 0, 'f', 2);
  ui->statusBar->showMessage(fpsString);
}

void SLStudio::receiveNewPointCloud(PointCloudConstPtr pointCloud) {
  // Display point cloud in widget
  if (ui->actionUpdatePointClouds->isChecked())
    ui->pointCloudWidget->updatePointCloud(pointCloud);

  if (trackerDialog->isVisible())
    trackerDialog->receiveNewPointCloud(pointCloud);
}

void SLStudio::closeEvent(QCloseEvent *event) {
  // Save main window geometry
  settings->setValue("geometry/mainwindow", saveGeometry());
  settings->setValue("state/mainwindow", saveState());

  // Store Geometry
  settings->setValue("geometry/histogram", histogramDialog->saveGeometry());
  settings->setValue("geometry/shading", shadingDialog->saveGeometry());
  settings->setValue("geometry/decoderUp", decoderUpDialog->saveGeometry());
  settings->setValue("geometry/decoderVp", decoderVpDialog->saveGeometry());

  // Store Visibility
  settings->setValue("visible/histogram", histogramDialog->isVisible());
  settings->setValue("visible/shading", shadingDialog->isVisible());
  settings->setValue("visible/decoderUp", decoderUpDialog->isVisible());
  settings->setValue("visible/decoderVp", decoderVpDialog->isVisible());

  // Store data for trackerDialog (temp)
  settings->setValue("geometry/trackerDialog", trackerDialog->saveGeometry());
  settings->setValue("visible/trackerDialog", trackerDialog->isVisible());

  event->accept();
}

SLStudio::~SLStudio() {
  delete ui;
  delete settings;
}

void SLStudio::onActionLoadCalibration() {
  QString fileName = QFileDialog::getOpenFileName(
      this, "Choose calibration file", QString(), "*.xml");
  if (!(fileName.length() == 0)) {
    CalibrationData calibration;
    calibration.load(fileName);
    calibration.save("calibration.xml");
  }
}

void SLStudio::onActionExportCalibration() {
  CalibrationData calibration;
  calibration.load("calibration.xml");
  //  Non native file dialog
  //    QFileDialog saveFileDialog(this, "Export Calibration", QString(),
  //    "*.xml;;*.slcalib;;*.m"); saveFileDialog.setDefaultSuffix("xml");
  //    saveFileDialog.exec();
  //    QString fileName = saveFileDialog.selectedFiles().first();
  //  Native file dialog
  QString selectedFilter;
  QString fileName =
      QFileDialog::getSaveFileName(this, "Export Calibration", QString(),
                                   "*.xml;;*.slcalib;;*.m", &selectedFilter);

  if (!(fileName.length() == 0)) {
    QFileInfo info(fileName);
    QString type = info.suffix();
    if (type == "") fileName.append(selectedFilter.remove(0, 1));
    calibration.save(fileName);
  }
}

void SLStudio::onActionAbout() {
  SLAboutDialog *aboutDialog = new SLAboutDialog(this);
  aboutDialog->exec();
}

// Debuggings slots for plotting on the main thread
void SLStudio::hist(const char *windowName, cv::Mat im, unsigned int x,
                    unsigned int y) {
  cvtools::hist(windowName, im, x, y);
}
void SLStudio::imshow(const char *windowName, cv::Mat im, unsigned int x,
                      unsigned int y) {
  cvtools::imshow(windowName, im, x, y);
}

void SLStudio::on_linearityTest_clicked() {
  unsigned int image_indices[] = {7, 49};
  // unsigned int image_indices[] = {7, 7};
  int num_photos = 3;
  std::vector<unsigned int> display_seq = {1, 0, 2};  // R->G->B

  // Initialise some parameters
  CameraTriggerMode triggerMode = triggerModeSoftware;
  QSettings settings("SLStudio");

  int camera_indice = 0;
  // Since we are displaying the same pattern until all images are captured
  // software trigger mode for the ROS camera is sufficient

  CameraROS camera{camera_indice, triggerModeSoftware};

  // CameraSpinnaker camera{camera_indice, triggerModeSoftware};

  // Set camera settings
  CameraSettings camSettings;
  camSettings.shutter = 16.667f;
  // camSettings.shutter = 8.333f;
  camSettings.gain = 0.0;
  camera.setCameraSettings(camSettings);

  // Grab single image
  /**
  camera.startCapture();
  CameraFrame frame;
  frame = camera.getFrame();
  // For RGB8 images
  cv::Mat frameCV(frame.height, frame.width, CV_8UC3, frame.memory);
  cv::cvtColor(frameCV, frameCV, cv::COLOR_RGB2BGR);

  frameCV = frameCV.clone();
  std::string filename = "initial.bmp";
  cv::imwrite(filename, frameCV);
  camera.stopCapture();

  cout << "Done" << endl;
  **/

  // Initialize projector (just set to versavis for now, only displays patterns
  // stored in flash)
  ProjectorLC4500Versavis projector{0};

  // Init projector
  auto is_hardware_triggered = std::make_shared<bool>(false);
  auto void_is_hardware_triggered =
      std::static_pointer_cast<void>(is_hardware_triggered);
  projector.loadParam("is_hardware_triggered", void_is_hardware_triggered);

  auto is_in_calibration_mode = std::make_shared<bool>(false);
  auto void_is_in_calibration_mode =
      std::static_pointer_cast<void>(is_in_calibration_mode);
  projector.loadParam("is_in_calibration_mode", void_is_in_calibration_mode);

  projector.init();

  std::cout << "Starting capture!" << std::endl;
  camera.startCapture();

  for (int i = image_indices[0]; i <= image_indices[1]; i++) {
    for (int j = 0; j < display_seq.size(); j++) {
      projector.display_8_bit_image(i, display_seq[j]);
      // Give projector some time to receive and execute change in pattern
      // display
      std::this_thread::sleep_for(std::chrono::milliseconds((int)(1000)));
      for (int k = 1; k <= num_photos; k++) {
        std::cout << "Capturing frame " << i << " pattern " << j
                  << " iteration " << k << std::endl;

        CameraFrame frame;
        frame = camera.getFrame();

        // Mono 8 Images
        cv::Mat frameCV(frame.height, frame.width, CV_8U, frame.memory);

        // For RGB8 images
        // cv::Mat frameCV(frame.height, frame.width, CV_8UC3, frame.memory);
        // cv::cvtColor(frameCV, frameCV, cv::COLOR_RGB2BGR);

        frameCV = frameCV.clone();
        std::string filename = "Intensity_" + std::to_string(i) + "_" +
                               std::to_string(j) + "_" + std::to_string(k) +
                               ".bmp";
        cv::imwrite(filename, frameCV);
      }
    }
  }

  camera.stopCapture();
  projector.displayBlack();
}

static void writePatterns(std::shared_ptr<Encoder> encoder_ptr,
                          std::string title, bool diamondPattern,
                          unsigned int screenRows, unsigned int screenCols,
                          cv::Mat map1, cv::Mat map2) {
  // Generate patterns and save
  for (unsigned int i = 0; i < encoder_ptr->getNPatterns(); i++) {
    cv::Mat pattern = encoder_ptr->getEncodingPattern(i);

    // general repmat
    pattern = cv::repeat(pattern, screenRows / pattern.rows + 1,
                         screenCols / pattern.cols + 1);
    pattern = pattern(cv::Range(0, screenRows), cv::Range(0, screenCols));

    // correct for lens distortion
    cv::remap(pattern, pattern, map1, map2, CV_INTER_CUBIC);

    if (diamondPattern) pattern = cvtools::diamondDownsample(pattern);

    std::string filename = title + "_" + std::to_string(i) + ".bmp";

    cv::imwrite(filename, pattern);
  }
}

void SLStudio::on_generatePatterns_clicked() {
  // Setup projector
  bool diamondPattern = true;
  unsigned int screenResX, screenResY;
  ProjectorLC4500Versavis::getLC4500ScreenRes(screenResX, screenResY);

  // Get number of rows and columns
  unsigned int screenCols, screenRows;
  if (diamondPattern) {
    screenCols = 2 * screenResX;
    screenRows = screenResY;
  } else {
    screenCols = screenResX;
    screenRows = screenResY;
  }

  // Lens correction and upload patterns to projector/GPU
  CalibrationData calibration;
  calibration.load("calibration.xml");
  cv::Mat map1, map2;
  cv::Size mapSize = cv::Size(screenCols, screenRows);
  cvtools::initDistortMap(calibration.Kp, calibration.kp, mapSize, map1, map2);

  // Create encoders
  auto encoder_2p1_horz_ptr = std::make_shared<EncoderPhaseShift2p1Tpu>(
      screenCols, screenRows, CodecDirHorizontal);
  auto encoder_2x3_horz_ptr = std::make_shared<EncoderPhaseShift2x3>(
      screenCols, screenRows, CodecDirHorizontal);
  auto encoder_2p1_vert_ptr = std::make_shared<EncoderPhaseShift2p1Tpu>(
      screenCols, screenRows, CodecDirVertical);
  auto encoder_2x3_vert_ptr = std::make_shared<EncoderPhaseShift2x3>(
      screenCols, screenRows, CodecDirVertical);

  // Write Patterns
  writePatterns(encoder_2p1_horz_ptr, "2p1_tpu_h", diamondPattern, screenRows,
                screenCols, map1, map2);
  writePatterns(encoder_2p1_vert_ptr, "2p1_tpu_v", diamondPattern, screenRows,
                screenCols, map1, map2);
  writePatterns(encoder_2x3_horz_ptr, "psp_tpu_h", diamondPattern, screenRows,
                screenCols, map1, map2);
  writePatterns(encoder_2x3_vert_ptr, "psp_tpu_v", diamondPattern, screenRows,
                screenCols, map1, map2);

  std::cout << "Generated patterns successfully" << std::endl;
}

void SLStudio::on_startProjector_clicked() {
  QSettings settings("SLStudio");

  CameraTriggerMode triggerMode = triggerModeHardware;
  projector_ptr = std::make_unique<ProjectorLC4500Versavis>(0);

  /**
 CodecDir dir_init =
     (CodecDir)settings.value("pattern/direction", CodecDirHorizontal).toInt();
 **/

  QString patternMode =
      settings.value("pattern/mode", "CodecPhaseShift3").toString();

  CodecDir dir_init = CodecDirVertical;
  auto display_horizontal_pattern = std::make_shared<bool>(
      (dir_init == CodecDirHorizontal || dir_init == CodecDirBoth) ? true
                                                                   : false);
  auto display_vertical_pattern = std::make_shared<bool>(
      (dir_init == CodecDirVertical || dir_init == CodecDirBoth) ? true
                                                                 : false);

  // Init projector
  auto is_hardware_triggered = std::make_shared<bool>(
      (triggerMode == triggerModeHardware) ? true : false);
  auto void_is_hardware_triggered =
      std::static_pointer_cast<void>(is_hardware_triggered);
  projector_ptr->loadParam("is_hardware_triggered", void_is_hardware_triggered);

  auto is_in_calibration_mode = std::make_shared<bool>(false);
  auto void_is_in_calibration_mode =
      std::static_pointer_cast<void>(is_in_calibration_mode);
  projector_ptr->loadParam("is_in_calibration_mode",
                           void_is_in_calibration_mode);

  auto is_2_plus_1_mode = std::make_shared<bool>(
      (patternMode == "CodecPhaseShift2p1Tpu") ? true : false);

  // auto is_2_plus_1_mode = std::make_shared<bool>(true);
  auto void_is_2_plus_1_mode = std::static_pointer_cast<void>(is_2_plus_1_mode);
  projector_ptr->loadParam("is_2_plus_1_mode", void_is_2_plus_1_mode);

  auto void_display_vertical_pattern =
      std::static_pointer_cast<void>(display_vertical_pattern);
  projector_ptr->loadParam("display_vertical_pattern",
                           void_display_vertical_pattern);

  auto void_display_horizontal_pattern =
      std::static_pointer_cast<void>(display_horizontal_pattern);
  projector_ptr->loadParam("display_horizontal_pattern",
                           void_display_horizontal_pattern);

  projector_ptr->init();
  projector_ptr->startProjection();
}

void SLStudio::on_stopProjector_clicked() {
  if (projector_ptr) {
    projector_ptr->displayBlack();
    projector_ptr.reset();
  }
}
