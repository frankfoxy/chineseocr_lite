#include "OcrLite.h"
#include "OcrUtils.h"
#include <stdarg.h> //windows&linux
#include <cstdio>

OcrLite::OcrLite() {}

OcrLite::~OcrLite() {
    if (isOutputResultTxt) {
        fclose(resultTxt);
    }
}

void OcrLite::setNumThread(int numOfThread) {
    dbNet.setNumThread(numOfThread);
    angleNet.setNumThread(numOfThread);
    crnnNet.setNumThread(numOfThread);
}

void OcrLite::initLogger(bool isConsole, bool isPartImg, bool isResultImg) {
    isOutputConsole = isConsole;
    isOutputPartImg = isPartImg;
    isOutputResultImg = isResultImg;
}

void OcrLite::enableResultTxt(const char *path, const char *imgName) {
    isOutputResultTxt = true;
    std::string resultTxtPath = getResultTxtFilePath(path, imgName);
    printf("resultTxtPath(%s)\n", resultTxtPath.c_str());
    resultTxt = fopen(resultTxtPath.c_str(), "w");
}

void OcrLite::initModels(const std::string &detPath, const std::string &clsPath,
                         const std::string &recPath, const std::string &keysPath) {
    Logger("=====Init Models=====\n");
    Logger("--- Init DbNet ---\n");
    dbNet.initModel(detPath);

    Logger("--- Init AngleNet ---\n");
    angleNet.initModel(clsPath);

    Logger("--- Init CrnnNet ---\n");
    crnnNet.initModel(recPath, keysPath);

    Logger("Init Models Success!\n");
}

void OcrLite::Logger(const char *format, ...) {
    if (!(isOutputConsole || isOutputResultTxt)) return;
    if (isOutputConsole){
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
    } 
    if (isOutputResultTxt) {
        va_list args;
        va_start(args, format);
        vfprintf(resultTxt, format, args);
        va_end(args);
    }
}

cv::Mat makePadding(cv::Mat &src, const int padding) {
    if (padding <= 0) return src;
    cv::Scalar paddingScalar = {255, 255, 255};
    cv::Mat paddingSrc;
    cv::copyMakeBorder(src, paddingSrc, padding, padding, padding, padding, cv::BORDER_ISOLATED, paddingScalar);
    return paddingSrc;
}

OcrResult OcrLite::detect(const char *path, const char *imgName,
                          const int padding, const int maxSideLen,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle) {
    std::string imgFile = getSrcImgFilePath(path, imgName);

    cv::Mat bgrSrc = imread(imgFile, cv::IMREAD_COLOR);//default : BGR
    cv::Mat originSrc;
    cvtColor(bgrSrc, originSrc, cv::COLOR_BGR2RGB);// convert to RGB
    int originMaxSide = (std::max)(originSrc.cols, originSrc.rows);
    int resize;
    //if (maxSideLen <= 0 || maxSideLen > originMaxSide) {
    if (maxSideLen <= 0 ) {
        resize = originMaxSide;
    } else {
        resize = maxSideLen;
    }
    //resize += 2*padding;
    cv::Rect paddingRect(padding, padding, originSrc.cols, originSrc.rows);
    cv::Mat paddingSrc = makePadding(originSrc, padding);
    ScaleParam scale = getScaleParam(paddingSrc, resize);
    OcrResult result;
    result = detect(path, imgName, paddingSrc, paddingRect, scale,
                    boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
    return result;
}

std::vector<cv::Mat> OcrLite::getPartImages(cv::Mat &src, std::vector<TextBox> &textBoxes,
                                            const char *path, const char *imgName) {
    std::vector<cv::Mat> partImages;
    for (unsigned int i = 0; i < textBoxes.size(); ++i) {
        cv::Mat partImg = getRotateCropImage(src, textBoxes[i].boxPoint);
        partImages.emplace_back(partImg);
        //OutPut DebugImg
        if (isOutputPartImg) {
            std::string debugImgFile = getDebugImgFilePath(path, imgName, i, "-part-");
            saveImg(partImg, debugImgFile.c_str());
        }
    }
    return partImages;
}

OcrResult OcrLite::detect(const char *path, const char *imgName,
                          cv::Mat &src, cv::Rect &originRect, ScaleParam &scale,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle) {

    cv::Mat textBoxPaddingImg = src.clone();
    int thickness = getThickness(src);

    Logger("=====Start detect=====\n");
    Logger("ScaleParam(sw:%d,sh:%d,dw:%d,dh:%d,%f,%f)\n", scale.srcWidth, scale.srcHeight,
           scale.dstWidth, scale.dstHeight,
           scale.ratioWidth, scale.ratioHeight);

    Logger("---------- step: dbNet getTextBoxes ----------\n");
    double startTime = getCurrentTime();
    std::vector<TextBox> textBoxes = dbNet.getTextBoxes(src, scale, boxScoreThresh, boxThresh, unClipRatio);
    double endDbNetTime = getCurrentTime();
    double dbNetTime = endDbNetTime - startTime;
    Logger("dbNetTime(%fms)\n", dbNetTime);

    for (unsigned int i = 0; i < textBoxes.size(); ++i) {
        Logger("TextBox[%d](+padding)[score(%f),[x: %d, y: %d], [x: %d, y: %d], [x: %d, y: %d], [x: %d, y: %d]]\n", i,
               textBoxes[i].score,
               textBoxes[i].boxPoint[0].x, textBoxes[i].boxPoint[0].y,
               textBoxes[i].boxPoint[1].x, textBoxes[i].boxPoint[1].y,
               textBoxes[i].boxPoint[2].x, textBoxes[i].boxPoint[2].y,
               textBoxes[i].boxPoint[3].x, textBoxes[i].boxPoint[3].y
               );
    }

    Logger("---------- step: drawTextBoxes ----------\n");
    drawTextBoxes(textBoxPaddingImg, textBoxes, thickness);

    //---------- getPartImages ----------
    std::vector<cv::Mat> partImages = getPartImages(src, textBoxes, path, imgName);

    Logger("---------- step: angleNet getAngles ----------\n");
    std::vector<Angle> angles;
    angles = angleNet.getAngles(partImages, path, imgName, doAngle, mostAngle);

    //Log Angles
    for (unsigned int i = 0; i < angles.size(); ++i) {
        Logger("angle[%d][index(%d), score(%f), time(%fms)]\n", i, angles[i].index, angles[i].score, angles[i].time);
    }

    Logger("---------- step: crnnNet getTextLine ----------\n");

    //Rotate partImgs
    for (unsigned int i = 0; i < partImages.size(); ++i) {
        if (angles[i].index == 0) {
            partImages.at(i) = matRotateClockWise180(partImages[i]);
        }
    }

    std::vector<TextLine> textLines = crnnNet.getTextLines(partImages, path, imgName);

    int txt_score_low = 0;
    //Log TextLines
    for (unsigned int i = 0; i < textLines.size(); ++i) {
        Logger("textLine[%d](%s)\n", i, textLines[i].text.c_str());
        std::ostringstream txtScores;
        for (unsigned int s = 0; s < textLines[i].charScores.size(); ++s) {
            if (s == 0) {
                txtScores << textLines[i].charScores[s];
            } else {
                txtScores << " ," << textLines[i].charScores[s];
            }
        }
        Logger("textScores[%d]{%s}\n", i, std::string(txtScores.str()).c_str());
        Logger("crnnTime[%d](%fms)\n", i, textLines[i].time);
    }

    std::vector<int> angleDeg;
    for (unsigned int i = 0; i < textBoxes.size(); ++i) {
        float x0 = textBoxes[i].boxPoint[0].x, y0 = textBoxes[i].boxPoint[0].y;
        float x1 = textBoxes[i].boxPoint[1].x, y1 = textBoxes[i].boxPoint[1].y;
        float x2 = textBoxes[i].boxPoint[3].x, y2 = textBoxes[i].boxPoint[3].y;

        float l0_sq = (x0 - x1) * (x0 - x1) + (y0 - y1) * (y0 - y1);
        float l1_sq = (x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2);
        float deg0 = 180 / 3.1415926 * atan2(y1 - y0, x1 - x0) + 180;
        float deg1 = 180 / 3.1415926 * atan2(y2 - y0, x2 - x0) + 180;

        float deg = l0_sq > l1_sq ? deg0 : deg1;
        int deg_int = 360 - (int)(deg + 0.5);

        if (angles[i].index != 0) {
            deg_int = (deg_int + 180) % 360;
        }
        if (textLines[i].text.length() < 2)
            continue;

        //float score = 0;
        //for (unsigned int j = 0; j < textLines[i].charScores.size(); j++) {
            //score += textLines[i].charScores[j];
        //}
        //score /= textLines[i].charScores.size();

        if (textLines[i].charScores[0] < 0.8 && textLines[i].charScores[1] < 0.8) {
            deg_int = (deg_int + 180) % 360;
        }

        Logger("Angle: %d (%.0f, %.0f, %.0f, %.0f) \n", deg_int, deg0, deg1, l0_sq, l1_sq);
        printf("Angle: %d\n", deg_int);
        angleDeg.emplace_back(deg_int);
    }

    std::vector<TextBlock> textBlocks;
    for (unsigned int i = 0; i < textLines.size(); ++i) {
        std::vector<cv::Point> boxPoint = std::vector<cv::Point>(4);
        int padding = originRect.x;//padding conversion
        boxPoint[0] = cv::Point(textBoxes[i].boxPoint[0].x - padding, textBoxes[i].boxPoint[0].y - padding);
        boxPoint[1] = cv::Point(textBoxes[i].boxPoint[1].x - padding, textBoxes[i].boxPoint[1].y - padding);
        boxPoint[2] = cv::Point(textBoxes[i].boxPoint[2].x - padding, textBoxes[i].boxPoint[2].y - padding);
        boxPoint[3] = cv::Point(textBoxes[i].boxPoint[3].x - padding, textBoxes[i].boxPoint[3].y - padding);
        TextBlock textBlock{boxPoint, textBoxes[i].score, angles[i].index, angles[i].score,
                            angles[i].time, angleDeg[i], textLines[i].text, textLines[i].charScores, textLines[i].time,
                            angles[i].time + textLines[i].time};
        textBlocks.emplace_back(textBlock);
    }

    double endTime = getCurrentTime();
    double fullTime = endTime - startTime;
    Logger("=====End detect=====\n");
    Logger("FullDetectTime(%fms)\n", fullTime);

    //cropped to original size
    cv::Mat rgbBoxImg, textBoxImg;

    if (originRect.x > 0 && originRect.y > 0) {
        textBoxPaddingImg(originRect).copyTo(rgbBoxImg);
    } else {
        rgbBoxImg = textBoxPaddingImg;
    }
    cvtColor(rgbBoxImg, textBoxImg, cv::COLOR_RGB2BGR);//convert to BGR for Output Result Img

    //Save result.jpg
    if (isOutputResultImg) {
        std::string resultImgFile = getResultImgFilePath(path, imgName);
        imwrite(resultImgFile, textBoxImg);
    }

    std::string strRes;
    for (unsigned int i = 0; i < textBlocks.size(); ++i) {
        strRes.append(textBlocks[i].text);
        strRes.append("\n");
    }

    return OcrResult{dbNetTime, textBlocks, textBoxImg, fullTime, strRes};
}
