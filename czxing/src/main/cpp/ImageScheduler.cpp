//
// Created by Devilsen on 2019-08-09.
//

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/types_c.h>
#include <src/BinaryBitmap.h>
#include "ImageScheduler.h"
#include "JNIUtils.h"

#define DEFAULT_MIN_LIGHT 70;

ImageScheduler::ImageScheduler(JNIEnv *env, MultiFormatReader *_reader,
                               JavaCallHelper *javaCallHelper) {
    this->env = env;
    this->reader = _reader;
    this->javaCallHelper = javaCallHelper;
    zbarScanner = new ImageScanner();
    zbarScanner->set_config(ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
    qrCodeRecognizer = new QRCodeRecognizer();
    stopProcessing.store(false);
    isProcessing.store(false);
}

ImageScheduler::~ImageScheduler() {
    DELETE(env);
    DELETE(reader);
    DELETE(javaCallHelper);
    DELETE(zbarScanner);
    DELETE(qrCodeRecognizer);
    frameQueue.clear();
    delete &isProcessing;
    delete &stopProcessing;
    delete &cameraLight;
    delete &prepareThread;
}

void *prepareMethod(void *arg) {
    auto scheduler = static_cast<ImageScheduler *>(arg);
    scheduler->start();
    return nullptr;
}

void ImageScheduler::prepare() {
    pthread_create(&prepareThread, nullptr, prepareMethod, this);
}

void ImageScheduler::start() {
    stopProcessing.store(false);
    isProcessing.store(false);
    frameQueue.setWork(1);

    while (true) {
        if (stopProcessing.load()) {
            break;
        }

        if (isProcessing.load()) {
            continue;
        }

        FrameData frameData;
        int ret = frameQueue.deQueue(frameData);
        if (ret) {
            isProcessing.store(true);
            preTreatMat(frameData);
            isProcessing.store(false);
        }
    }
}

void ImageScheduler::stop() {
    isProcessing.store(false);
    stopProcessing.store(true);
    frameQueue.setWork(0);
    frameQueue.clear();
}

void
ImageScheduler::process(jbyte *bytes, int left, int top, int cropWidth, int cropHeight,
                        int rowWidth,
                        int rowHeight) {
    if (isProcessing.load()) {
        return;
    }

    FrameData frameData;
    frameData.left = left;
    frameData.top = top;
    frameData.cropWidth = cropWidth;
    if (top + cropHeight > rowHeight) {
        frameData.cropHeight = rowHeight - top;
    } else {
        frameData.cropHeight = cropHeight;
    }
    if (frameData.cropHeight < frameData.cropWidth) {
        frameData.cropWidth = frameData.cropHeight;
    }
    frameData.rowWidth = rowWidth;
    frameData.rowHeight = rowHeight;
    frameData.bytes = bytes;

    frameQueue.enQueue(frameData);
//    LOGE("frame data size : %d", frameQueue.size());
}

/**
 * 预处理二进制数据
 */
void ImageScheduler::preTreatMat(const FrameData &frameData) {
    try {
        LOGE("start preTreatMat...");

        Mat src(frameData.rowHeight + frameData.rowHeight / 2,
                frameData.rowWidth, CV_8UC1,
                frameData.bytes);

        Mat gray;
        cvtColor(src, gray, COLOR_YUV2GRAY_NV21);

        if (frameData.left != 0) {
            gray = gray(
                    Rect(frameData.left, frameData.top, frameData.cropWidth, frameData.cropHeight));
        }

        // 分析亮度，如果亮度过低，不进行处理
        analysisBrightness(gray);
        if (cameraLight < 40) {
            return;
        }
        decodeGrayPixels(gray);
    } catch (const std::exception &e) {
        LOGE("preTreatMat error...");
    }
}

void ImageScheduler::decodeGrayPixels(const Mat &gray) {
    LOGE("start GrayPixels...");

    Mat mat;
    rotate(gray, mat, ROTATE_90_CLOCKWISE);

    Result result = decodePixels(mat);

    if (result.isValid()) {
        javaCallHelper->onResult(result);
    }
//    else if (result.isNeedScale()) {
//        LOGE("is need scale image...");
//        std::vector<ResultPoint> points = result.resultPoints();
//        ResultPoint topLeft = points[1];
//        ResultPoint topRight = points[2];
//        ResultPoint bottomLeft = points[0];
//
//        int left = static_cast<int>(topLeft.x()) - 3 * 20;
//        int top = static_cast<int>(topLeft.y()) - 3 * 20;
//        int width = static_cast<int>(topRight.x() - topLeft.x()) + 3 * 25;
//        int height = static_cast<int>(bottomLeft.y() - topLeft.y()) + 3 * 25;
//
//        LOGE("left = %d, top = %d, width = %d, height = %d", left, top, width, height);
//
//        mat = mat(Rect(left, top, width, height));
//        imwrite("/storage/emulated/0/scan/scale.jpg", mat);
//        Result result1 = decodePixels(mat);
//
//        if (result.isValid()) {
//            javaCallHelper->onResult(result);
//        } else {
//            decodeThresholdPixels(gray);
//        }
//    }
    else {
        decodeThresholdPixels(gray);
    }
}

void ImageScheduler::decodeThresholdPixels(const Mat &gray) {
    LOGE("start ThresholdPixels...");

    Mat mat;
    rotate(gray, mat, ROTATE_180);

    // 提升亮度
    if (cameraLight < 80) {
        mat.convertTo(mat, -1, 1.0, 30);
    }

    threshold(mat, mat, 50, 255, CV_THRESH_OTSU);

    Result result = decodePixels(mat);
    if (result.isValid()) {
        javaCallHelper->onResult(result);
    } else {
        decodeAdaptivePixels(gray);
    }
}

void ImageScheduler::decodeAdaptivePixels(const Mat &gray) {
    LOGE("start AdaptivePixels...");

    Mat mat;
    rotate(gray, mat, ROTATE_90_COUNTERCLOCKWISE);

    // 降低图片亮度
    Mat lightMat;
    mat.convertTo(lightMat, -1, 1.0, -60);

    adaptiveThreshold(lightMat, lightMat, 255, ADAPTIVE_THRESH_MEAN_C,
                      THRESH_BINARY, 55, 3);

    Result result = decodePixels(lightMat);
    if (result.isValid()) {
        javaCallHelper->onResult(result);
    } else {
        decodeZBar(gray);
//        recognizerQrCode(gray);
    }
}

void ImageScheduler::decodeZBar(const Mat &gray) {
    auto width = static_cast<unsigned int>(gray.cols);
    auto height = static_cast<unsigned int>(gray.rows);

    const void *raw = gray.data;
    Image image(width, height, "Y800", raw, width * height);
    int n = zbarScanner->scan(image);

    // extract results
    if (n > 0) {
        Image::SymbolIterator symbol = image.symbol_begin();
        LOGE("zbar Code Data = %s", symbol->get_data().c_str());

        if (symbol->get_type() == zbar_symbol_type_e::ZBAR_QRCODE) {
            Result result(DecodeStatus::NoError);
            result.setFormat(BarcodeFormat::QR_CODE);
            result.setText(ANSIToUnicode(symbol->get_data()));
            javaCallHelper->onResult(result);
            image.set_data(nullptr, 0);
        }
    } else {
        image.set_data(nullptr, 0);
        recognizerQrCode(gray);
    }
}

void ImageScheduler::recognizerQrCode(const Mat &mat) {
    LOGE("start recognizerQrCode...");

    cv::Rect rect;
    qrCodeRecognizer->processData(mat, &rect);
    if (rect.empty()) {
        return;
    }

    ResultPoint point1(rect.tl().x, rect.tl().y);
    ResultPoint point2(rect.br().x, rect.tl().y);
    ResultPoint point3(rect.tl().x, rect.br().y);

    std::vector<ResultPoint> points;
    points.push_back(point1);
    points.push_back(point2);
    points.push_back(point3);

    Result result(DecodeStatus::NotFound);
    result.setResultPoints(std::move(points));

    javaCallHelper->onResult(result);

    LOGE("end recognizerQrCode...");

}

Result ImageScheduler::decodePixels(const Mat &mat) {
    try {
//        int width = mat.cols;
//        int height = mat.rows;
//        Mat resultMat(height, width, CV_8UC1, pixels);
//        imwrite("/storage/emulated/0/scan/result.jpg", resultMat);

        auto binImage = BinaryBitmapFromBytesC1(mat.data, 0, 0, mat.cols, mat.rows);
        return reader->read(*binImage);
    } catch (const std::exception &e) {
        ThrowJavaException(env, e.what());
    } catch (...) {
        ThrowJavaException(env, "Unknown exception");
    }

    return Result(DecodeStatus::NotFound);
}

bool ImageScheduler::analysisBrightness(const Mat &gray) {
    LOGE("start analysisBrightness...");

    // 平均亮度
    Scalar scalar = mean(gray);
    cameraLight = scalar.val[0];
    LOGE("平均亮度 %lf", cameraLight);
    // 判断在时间范围 AMBIENT_BRIGHTNESS_WAIT_SCAN_TIME * lightSize 内是不是亮度过暗
    bool isDark = cameraLight < DEFAULT_MIN_LIGHT;
    javaCallHelper->onBrightness(isDark);

    return isDark;
}

Result *ImageScheduler::analyzeResult() {
    Result *result = nullptr;

    return result;
}

void ImageScheduler::decodeZBar(const jobject &gray) {
//    auto width = static_cast<unsigned int>(gray.cols);
//    auto height = static_cast<unsigned int>(gray.rows);
//
//    const void *raw = gray.data;
//    Image image(width, height, "Y800", raw, width * height);
//    ImageScanner scanner;
//    int n = scanner.scan(image);
//
//    // extract results
//    if (image.symbol_begin() == image.symbol_end()) {
//        image.set_data(nullptr, 0);
//        recognizerQrCode(gray);
//        return;
//    }
//
//    Image::SymbolIterator symbol = image.symbol_begin();
//    for (; symbol != image.symbol_end(); ++symbol) {
//        LOGE("zbar Code Type = %s , Data = %s", symbol->get_type_name().c_str(),
//             symbol->get_data().c_str());
//
//        if (symbol->get_type() == zbar_symbol_type_e::ZBAR_QRCODE){
//            Result result(DecodeStatus::NoError);
//            result.setFormat(BarcodeFormat::QR_CODE);
//            result.setText(ANSIToUnicode(symbol->get_data()));
//            javaCallHelper->onResult(result);
//        }
//    }
//    image.set_data(nullptr, 0);
}

Result ImageScheduler::readBitmap(jobject bitmap, int left, int top, int width, int height) {
    auto binImage = BinaryBitmapFromJavaBitmap(env, bitmap, left, top, width, height);
    if (!binImage) {
        LOGE("create binary bitmap fail");
        return Result(DecodeStatus::NotFound);
    }
//    Result result = reader->read(*binImage);
//    if (!result.isValid()){
//
//    }
    return reader->read(*binImage);
}



