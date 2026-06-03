#ifndef Q565_H
#define Q565_H

#include <QRgb>
#include <QMap>
#include <QDebug>
#include <QtEndian>
#include <QImage>
#include <QBitArray>
#include <QFile>

namespace Q565_Encoder {
    constexpr unsigned short MAX_COLORS = 64;
    // Anti-artefacts : désactive les opcodes 2 octets (LUMA / INDEX_DIFF), suspectés
    // de désynchroniser le décodeur du firmware sur certaines frames (corruption qui
    // se propage → bandes / bloc en bas). RUN, INDEX et DIFF (1 octet) restent actifs.
    constexpr bool Q565_USE_COMPLEX_OPS = false;
    constexpr unsigned short Q565_OP_RGB565 =  0xFE; // 0b11111110
    constexpr unsigned short Q565_OP_INDEX  = 0b00000000;
    constexpr unsigned short Q565_OP_INDEX_DIFF = 0b10100000;
    constexpr unsigned short Q565_OP_DIFF   = 0b01000000;
    constexpr unsigned short Q565_OP_LUMA   = 0b10000000;
    constexpr unsigned short Q565_OP_RUN    = 0b11000000;
    constexpr unsigned short Q565_TYPE_MASK = 0b11000000;
    constexpr unsigned short Q565_DATA_MASK = 0b00111111;
    constexpr unsigned short Q565_OP_END = 0xFF;
    const int QRGB_ARRAY_SIZE = 1228809; // 1MB
    constexpr char Q565_FORMAT_TAG[] = "q565";

    // get565Pixel (ancienne version) supprimee : division entiere cassee
    // (qRed/255 == 0 sauf a 255) et de toute facon inutilisee — l'encodeur
    // utilise get565Pixel2 ci-dessous.
    static quint16 get565Pixel2(QRgb rgb){ // assumed 8,8,8 bit rgb
        auto red = (qRed(rgb) * 249 + 1014) >> 11 ;
        auto green = (qGreen(rgb) * 253 + 505) >> 10 ;
        auto blue = (qBlue(rgb) * 249 + 1014) >> 11;
        return  red << 11 | green << 5 | blue;
    }
    static quint8 wrapping_add(quint8 a, quint8 b, quint8 max) {
        quint16 c{a};
        c += b;
        if(c >= max) {
            c -= max;
        }
        return (quint8)(c);
    }

    static quint8 wrapping_sub(quint8 a, quint8 b, quint8 max) {
        qint16 diff = {a};
        diff -= b;
        if(diff < 0) {
            diff += max;
        }
        return quint8(diff);
    }

    static quint16 decodePixel(quint8 low, quint8 high)
    {
        return ((high << 8) | low);
    }

    static quint8 pixelHash(quint16 pixel) {
        unsigned short a = (pixel & 0b1111111100000000) >> 8;
        unsigned short b = pixel & 0b0000000011111111;
        return (a+b) & 0b111111; // % 64
    }
    static bool wouldEncodeColorTable(quint16 pixel, QMap<quint8, quint16> & colorTable, quint8& colorHash) {
        // check if the colorTable already exists
        colorHash = pixelHash(pixel);
        if(!colorTable.contains(colorHash)) {
            return  (colorTable.size() < MAX_COLORS);
        }
        quint16 existingColor = colorTable.value(colorHash);
        return existingColor != pixel;
    }

    static quint8 encodeColorTable(quint16 pixel, QMap<quint8, quint16> & colorTable) {
        // check if the colorTable already exists
        quint8 colorHash = pixelHash(pixel);
        if(!colorTable.contains(colorHash)) {
            if(colorTable.size() < MAX_COLORS) {
                colorTable.insert(colorHash, pixel);
            } else {
                qDebug() << "Reached Color Array Limit" << colorHash << " for pixel " << QString::number(pixel, 16) << " - Colors: " << colorTable.size() ;
                return MAX_COLORS + 1;
            }
        } else {
            quint16 existingColor = colorTable.value(colorHash);
            if(existingColor != pixel) {
                //qDebug() << "Color Table - Swapping: "<< "Red: " << (existingColor >> 11) << " Green: " << ((existingColor >> 5) & 0b111111) << " Blue: " << (existingColor & 0b11111);
                //qDebug() << "Color Table - For: "<< "Red: " << (pixel >> 11) << " Green: " << ((pixel >> 5) & 0b111111) << " Blue: " << (pixel & 0b11111);
                colorTable.remove(colorHash);
                colorTable.insert(colorHash, pixel);
            } // else return colorHash
        }
        return colorHash;
    }

    static void encodeFullPixel(QByteArray* encodeArray, quint16 currentPixel) {
        encodeArray->append(Q565_Encoder::Q565_OP_RGB565); // RGB565 (full pixel marker)
        encodeArray->append((currentPixel & Q565_Encoder::Q565_OP_END)); // 565 Low byte
        encodeArray->append((currentPixel >> 8) & Q565_Encoder::Q565_OP_END); // 565 high byte
    }

    static bool decodeByte(unsigned char byte, unsigned char& type, unsigned char& data) {
        if(byte == Q565_OP_END) {
            return false;
        }
        type = byte & Q565_TYPE_MASK;
        data = byte & Q565_DATA_MASK;
        return true;
    };

    static qint8 calculateDiff(quint8 newValue, quint8 previousValue, quint8 max)
    {
        if(newValue == previousValue) {
            return 0;
        }
        qint8 diff = newValue - previousValue;
        qint8 wrapping_diff;
        if(diff < 0) {
            wrapping_diff = (diff + max);
        }else {
            wrapping_diff = (diff - max);
        }
        if(abs(diff) < abs(wrapping_diff)) {
            return diff;
        } else {
            return wrapping_diff;
        }
        //
    }
    static bool encodeDiff(QByteArray* encodeArray, quint16 r_new, quint16 g_new, quint16 b_new, quint16 previous) {

        quint8 r_prev, g_prev, b_prev  = -1;
        qint8 r_diff, g_diff, b_diff;
        r_prev = (previous >> 11) & 0b11111;
        g_prev = (previous >> 5)  & 0b111111;
        b_prev = previous         & 0b11111;

        r_diff = calculateDiff(r_new, r_prev , 32) + 2;
        g_diff = calculateDiff(g_new, g_prev, 64) + 2;
        b_diff = calculateDiff(b_new, b_prev, 32) + 2;
        // prefer diff over color table
        if ((r_diff <= 3 && r_diff >= 0) && (g_diff <= 3 && g_diff >= 0) && (b_diff <= 3 && b_diff >= 0) ){ // within 2 bit difference, Create a OP_DIFF Byte
            encodeArray->append(Q565_OP_DIFF | r_diff << 4 | g_diff << 2 | b_diff);
            return true;
        }
        return false;
    }
    static bool encodeIndexDiffed(QByteArray* encodeArray, quint16 r, quint16 g, quint16 b, QMap<quint8, quint16>& colorTable)
    {
        // if(wasAdded) {
        //     return false;
        // }
        // check through the colors in the color array
        qint8 r_diff, g_diff, b_diff;
        quint8 r_prev, g_prev, b_prev;
        quint16 color;
        QMapIterator<quint8, quint16> i(colorTable);

        while (i.hasNext()) {
            i.next();
            // get the diffs,
            color = i.value();
            r_diff = calculateDiff(r, (color >> 11), 32) + 2;
            g_diff = calculateDiff(g, ((color >> 5) & 0b111111), 64) + 4;
            b_diff = calculateDiff(b, color & 0b11111, 32) + 2;
            if(    r_diff <= 3 && r_diff >= 0
                && g_diff <= 7 && g_diff >= 0
                && b_diff <= 3 && b_diff >= 0) { // Can be encoded
                quint8 byte1 = Q565_OP_INDEX_DIFF | (g_diff << 2) | r_diff;
                quint8 byte2 = (b_diff << 6) | i.key();
                //qDebug() << "Encoding index diff " << "Red: " << r << " Green: " << g << " Blue: " << b;
                //qDebug() << "Index Diff Bytes: [0](" << QString::number(byte1, 2) <<  ") [1](" << QString::number(byte2, 2) << ")";
                encodeArray->append(byte1);
                encodeArray->append(byte2);
                return true;
            }
        }
        return false;
    }

    static bool encodeLuma(QByteArray* encodeArray, quint16 previous, quint16 r, quint16 g, quint16 b)
    {
        // convert from luma
        quint8 r_prev, g_prev, b_prev  = -1;
        qint8 rg_diff, g_diff, bg_diff;
        r_prev = (previous >> 11) & 0b11111;
        g_prev = (previous >> 5)  & 0b111111;
        b_prev = previous         & 0b11111;
        g_diff = calculateDiff(g, g_prev, 64);
        rg_diff = calculateDiff(r, r_prev , 32) - g_diff;
        bg_diff = calculateDiff(b, b_prev, 32)  - g_diff;
        if ((rg_diff >= -8 && rg_diff <= 7) && (g_diff >= -16 && g_diff <= 15) && (bg_diff >= -8 && bg_diff <= 7) ){ // within 2 bit difference, Create a OP_DIFF Byte
            encodeArray->append(Q565_OP_LUMA | ((g_diff + 16) & 0b11111) );
            encodeArray->append(((rg_diff+8) << 4) | (bg_diff+ 8));
            return true;
        }
        return false;
    }

    static void encodePixel(QByteArray* encodeArray,  quint16 r, quint16 g, quint16 b, quint16 previous, quint16 current, QMap<quint8, quint16>& colorTable)
    {
        quint8 colorHash;
        bool wouldAdd = wouldEncodeColorTable(current, colorTable, colorHash);
        if(!wouldAdd && colorHash < MAX_COLORS) {
            encodeArray->append(colorHash); // 565_OP_INDEX
            return;
        }
        if(Q565_USE_COMPLEX_OPS && encodeLuma(encodeArray, previous, r, g, b)) {

        } else if(Q565_USE_COMPLEX_OPS && encodeIndexDiffed(encodeArray, r, g, b, colorTable)) {
            encodeColorTable(current, colorTable);
        } else{ // littéral RGB565 (op simple et robuste)
            encodeFullPixel(encodeArray, current);
        }
        if(wouldAdd) {
            encodeColorTable(current, colorTable);
        }
    }

    static QByteArray encode(const QImage& image, QByteArray* encodeArray = nullptr) {
        // Correctif fuite memoire : l'ancien code faisait `new QByteArray` sans
        // jamais le liberer (~1,2 Mo fuites par frame). On utilise un buffer
        // local quand l'appelant n'en fournit pas.
        QByteArray localBuffer;
        if(encodeArray == nullptr) {
            localBuffer.reserve(QRGB_ARRAY_SIZE);
            encodeArray = &localBuffer;
        } else {
            encodeArray->resize(0); // reutilise le buffer prealloue fourni par l'appelant
        }
        QImage convertedImage = image;//convertToFormat(QImage::Format_RGB16);
        encodeArray->append(Q565_FORMAT_TAG); // write the magic header
        // qDebug() << "Encoding image " << convertedImage.height() << "x" << convertedImage.width();
        quint16 w16(convertedImage.width());
        quint16 h16(convertedImage.height());
        // encode in little endian, 0xFF
        encodeArray->append(w16 & 0xFF);
        encodeArray->append(w16 >> 8);
        encodeArray->append(h16 & 0xFF);
        encodeArray->append(h16 >> 8);
        quint16 previous16 = 0;
        quint8 r_new, g_new, b_new;
        quint16 currentPixel = 0;
        QMap<quint8, quint16> colorTable;
        colorTable.insert(0,0);
        colorTable.insert(63, 0xFFFF); // black
        //colorTable.clear();
        bool wasAddedToTable = false;
        quint8 colorIndex{0};
        int rg_diff, bg_diff;
        unsigned char run{0};
        QRgb rgb{0};
        QRgb previousRgb{0};
        const QRgb* line = nullptr;
        quint32 pixelsWrittenOfOneColor = 0;
        for(int y = 0; y < convertedImage.height(); ++y) {
            line = reinterpret_cast<const QRgb*>(convertedImage.constScanLine(y));
            for(int x = 0; x < convertedImage.width(); ++x) {
                rgb = line[x];

                /*** Encode entire image using next two lines ***/
                // currentPixel = get565Pixel2(rgb);
                //encodeFullPixel(encodeArray, currentPixel);
                /*** End 'Encode Full Pixel' ***/

                if((previousRgb == rgb) && ( y > 0 || x > 0) ) { // if diff is 0 for rgb!
                    if( run > 61) { // 62
                        pixelsWrittenOfOneColor += run;
                        run -= 1;
                        //qDebug() << "Encoding run of " << run << "of pixel " << QBitArray::fromBits((char*)&currentPixel, 16);
                        encodeArray->append(Q565_Encoder::Q565_OP_RUN | run); // End the current run
                        run = 1;
                    } else {
                        ++run;
                        continue;
                    }
                } else {
                    currentPixel = get565Pixel2(rgb);
                    if(run >= 1) { // existing run going, finish it up
                        pixelsWrittenOfOneColor += run;
                        run -= 1; // decrement by two as the initial value added 1
                        encodeArray->append(Q565_Encoder::Q565_OP_RUN | run); // End the current run
                        run = 0; // reset
                    }
                    r_new = (currentPixel >> 11) & 0b11111;
                    g_new = (currentPixel >> 5)  & 0b111111;
                    b_new = currentPixel         & 0b11111;
                    // prefer diff over color table
                    if (!encodeDiff(encodeArray, r_new, g_new, b_new, previous16)){ // within 2 bit difference, Create a OP_DIFF Byte
                        encodePixel(encodeArray, r_new, g_new, b_new, previous16, currentPixel, colorTable);
                    }
                }
                previous16 = currentPixel;
                previousRgb = rgb;
            }
        }
        if(run > 1) { // existing run going, finish it up
            --run; // decrement run
            encodeArray->append(Q565_Encoder::Q565_OP_RUN | run); // End the current run
        }
        encodeArray->append(Q565_Encoder::Q565_OP_END);
        return (*encodeArray);
    }
    static void writePixel(QImage& image, QRgb rgb, int& x, int& y) {
        // todo convert rgb16 to RGBA32
        if(x >= image.width()) {
            x = 0;
            ++y;
        }
        image.setPixel(x++,y, rgb);

    }
    static void writeRun(QImage& image, QRgb rgb, int& x, int& y, unsigned short count) {
        for(int i = 0; i < count; i++) {
            writePixel(image, rgb, x, y);
        }
    }
    static QRgb convertToQRgb(quint16 rgb16) {

        QRgb qrgb;
        quint8 r,g,b;
        r = (rgb16 & 0b1111100000000000) >> 11;
        g = (rgb16 & 0b0000011111100000) >> 5;
        b = (rgb16 & 0b0000000000011111);
        r = (r * 255)/ 31;
        g = (g * 255)/ 63;
        b = (b * 255)/ 31;
        return 0xFF000000 | r << 16 | g << 8 | b;
    }
    static void printPixelData(quint16 rgb, quint8 colorHash, quint16 x, quint16 y)
    {
        qDebug() << "Added color hash" << colorHash << " for pixel "
                  << " Red: " << (rgb >> 11) << " Green: " << ((rgb >> 6) & 0b111111) << " Blue: " << (rgb & 0b11111)
                  << " x: " << x << " y: " << y;
    }
    static quint8 adjustPixelByLumaDiff(quint8 value, qint8 valueDiff, qint8 max, quint8 bias)
    {
        if(valueDiff == 0) {
            return value;
        }
        if(valueDiff <= (bias-1)) {
            qint8 valueAbs(bias - valueDiff);
            value = (max - valueAbs);
        } else {
            qint8 valueBiased(valueDiff - bias);
            if(valueBiased == (bias - 1)) {
                if((value + (bias - 1)) == max) {
                    value = 0;
                }
                else {
                    value += (bias - 1);
                }
            }
        }
        return value;
    }
    static quint8 adjustPixelByDiff(quint8 value, qint8 valueDiff, qint8 max)
    {
        if(valueDiff == 0) {
            return value;
        }
        if(valueDiff <  0) {
            value = wrapping_sub(value, -(valueDiff), max);
        } else {
            value = wrapping_add(value, valueDiff, max);
        }
        return value;
    }
    static quint16 getPixelDiff(quint16 previous, quint8 difference)
    {
        // get previous color values
        quint8 r = (previous & 0b1111100000000000) >> 11;
        quint8 g = (previous & 0b0000011111100000) >> 5;
        quint8 b = (previous & 0b0000000000011111);

        // get difference data from 8 bits
        qint8 rDiff = ((difference >> 4 ) & 0b11) - 2;
        qint8 gDiff = ((difference >> 2 ) & 0b11) - 2;
        qint8 bDiff = (difference & 0b11) -2;
        r = adjustPixelByDiff(r, rDiff, 32);
        g = adjustPixelByDiff(g, gDiff, 64);
        b = adjustPixelByDiff(b, bDiff, 32);
        quint16 pixel{r};
        pixel = pixel << 6;
        pixel |= g;
        return pixel << 5 | b;
    }

    static quint16  getPixelLuma(quint16 previous, quint8 luma1, quint8 luma2)
    {
        // get previous color values
        quint8 r = (previous & 0b1111100000000000) >> 11;
        quint8 g = (previous & 0b0000011111100000) >> 5;
        quint8 b = (previous & 0b0000000000011111);

        qint8 gDiff = (luma1 & 0b11111);
        gDiff -= 16;
        qint8 rgDiff = (luma2 >> 4) & 0x0F;
        rgDiff -= 8;
        qint8 bgDiff = luma2 & 0x0F;
        bgDiff -= 8;
        qint8 rDiff = (rgDiff + gDiff);
        qint8 bDiff = (bgDiff + gDiff);
        r = (r + rDiff) & 0b11111;
        g = (g + gDiff) & 0b111111;
        b = (b + bDiff) & 0b11111;
        return ((r << 11) | (g << 5) | b);
    }

    static quint16 getPixelIndexedDiff(quint16 indexedColor, quint8 indexed1, quint8 indexed2)
    {
        quint8 r = (indexedColor & 0b1111100000000000) >> 11;
        quint8 g = (indexedColor & 0b0000011111100000) >> 5;
        quint8 b = (indexedColor & 0b0000000000011111);
        qint8 rDiff = (indexed1 & 0b00000011) - 2;
        qint8 gDiff = ((indexed1 & 0b00011100) >> 2) - 4;
        qint8 bDiff = ((indexed2 & 0b11000000) >> 6) - 2;

        r = adjustPixelByDiff(r, rDiff, 32);
        g = adjustPixelByDiff(g, gDiff, 64);
        b = adjustPixelByDiff(b, bDiff, 32);
        quint16 pixel{r};
        pixel = pixel << 6;
        pixel |= g;
        return pixel << 5 | b;
    }

    enum Q565StatementType {
        NO_STATEMENT = 0,
        HEADER_STATEMENT,
        PIXEL_STATEMENT,
        INDEX_STATEMENT,
        DIFF_STATEMENT,
        DIFF_INDEX_STATEMENT,
        LUMA_STATEMENT,
        RUN_STATEMENT,
        END_STATEMENT,
        UNKNOWN_STATEMENT
    };

    class Q565Statement {
        public:
            Q565StatementType  type;
            quint16 data;
            quint32 fullPixel;
            quint16 q565Pixel;
        // make copy constructor etc
            Q565Statement() : type(NO_STATEMENT), data(0), q565Pixel(0), fullPixel(0) {}
            Q565Statement(const Q565Statement& rhs) : type(rhs.type), data(rhs.data),q565Pixel(rhs.q565Pixel), fullPixel(rhs.fullPixel)
            {
                this->q565Pixel = rhs.q565Pixel;
            }
            Q565Statement& operator=(const Q565Statement&& rhs)
            {
                this->type = rhs.type;
                this->data = rhs.data;
                this->fullPixel = rhs.fullPixel;
                this->q565Pixel = rhs.q565Pixel;
                return (*this);
            }
            Q565Statement& operator=(const Q565Statement& rhs)
            {
                this->type = rhs.type;
                this->data = rhs.data;
                this->fullPixel = rhs.fullPixel;
                this->q565Pixel = rhs.q565Pixel;
                return (*this);
            }
            ~Q565Statement(){
                this->type = NO_STATEMENT;
                this->data = 0;
                this->fullPixel = 0;
                this->q565Pixel = 0;
            }

    };

    static QList<Q565Statement> generateStatementListModel(QString q565EncodedFilePath)
    {
        QList<Q565Statement> generatedStatements; // initializes  zero items
        char buffer[4];
        unsigned char readChar;
        // load the raw data
        quint16 width;
        quint16 height;
        quint8 type;
        quint8 data;
        int x{0},y{0};
        quint16 rgb = 0;
        quint8  allocatedColors{0};
        quint8 colorIndex{0};
        QMap<quint8, quint16> colorTable;
        colorTable.clear();
        colorTable.insert(0,0);
        colorTable.insert(63, 0xFFFF); // black
        if (QFile::exists(q565EncodedFilePath)) {
            QFile q565EncodedFile(q565EncodedFilePath);
            if(q565EncodedFile.open(QIODevice::ReadOnly)) {
                // look for header
                // decode header
                // begin decoding Byte stream into generatedStatements
                q565EncodedFile.read(buffer, 4);
                buffer[2] = 0;
                buffer[3] = 0;
                if(q565EncodedFile.read(buffer, 2) == 2) {
                    width = (buffer[0] & Q565_Encoder::Q565_OP_END);
                    width |= (quint16(buffer[1]) << 8);// sent as little endian
                }
                else { return generatedStatements; }
                if(q565EncodedFile.read(buffer, 2) == 2) {
                    height = (buffer[0] & Q565_Encoder::Q565_OP_END);
                    height |= (quint16(buffer[1]) << 8);// sent as little endian
                }
                else { return generatedStatements; }
                Q565Statement statement;
                statement.type = HEADER_STATEMENT;
                statement.q565Pixel = width;
                statement.data = height;
                generatedStatements.append(statement);
                // until out of bytes
                while(q565EncodedFile.read(reinterpret_cast<char*>(&readChar), 1) == 1) {
                    Q565_Encoder::decodeByte(readChar, type, data);
                    switch(type) {
                        case Q565_Encoder::Q565_OP_INDEX: {
                            // data is an index for rgb (0..63)
                            if(colorTable.contains(data)) {
                                rgb = colorTable.value(data);
                                statement.type = INDEX_STATEMENT;
                                statement.data = data;
                                statement.q565Pixel = rgb;
                                statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                generatedStatements.append(statement);
                            } else {
                                qDebug() << "Color index requested not in the color table" << data << " X: " << x << " Y: " << y;
                                qDebug() << "Color Table has " << colorTable.size() << " colors";
                            }
                            break;
                        }
                        case Q565_Encoder::Q565_OP_DIFF: {
                            rgb = Q565_Encoder::getPixelDiff(rgb, data);
                            statement.type = DIFF_STATEMENT;
                            statement.data = data;
                            statement.q565Pixel = rgb;
                            statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                            generatedStatements.append(statement);
                            break;
                        }
                        case Q565_Encoder::Q565_OP_LUMA: {
                            if(q565EncodedFile.read(reinterpret_cast<char*>(&readChar), 1) == 1) {
                                if((data & 0b00100000) == 0) { // handle luma
                                    rgb = Q565_Encoder::getPixelLuma(rgb, data, readChar);
                                    statement.type = LUMA_STATEMENT;
                                    statement.data = data;
                                    statement.q565Pixel = rgb;
                                    statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                    generatedStatements.append(statement);
                                    Q565_Encoder::encodeColorTable(rgb, colorTable);
                                } else { // indexed diff
                                    int colorHash = readChar & 0b111111;
                                    if(colorTable.contains(colorHash)) {
                                        quint16 color = colorTable.value(colorHash);
                                        rgb = Q565_Encoder::getPixelIndexedDiff(color, data, readChar);
                                        statement.type = DIFF_INDEX_STATEMENT;
                                        statement.data = colorHash;
                                        statement.q565Pixel = rgb;
                                        statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                        generatedStatements.append(statement);
                                        Q565_Encoder::encodeColorTable(rgb, colorTable);
                                    }
                                }
                            } else {
                                qDebug() << "ERROR: Failed to fetch luma second byte";
                                //return false;
                            }
                            break;
                        }
                        case Q565_Encoder::Q565_OP_RUN: {
                            if(readChar == Q565_Encoder::Q565_OP_END) {
                                statement.type = END_STATEMENT;
                                statement.data = data;
                                statement.q565Pixel = rgb;
                                statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                generatedStatements.append(statement);
                            }else if(readChar == Q565_Encoder::Q565_OP_RGB565) {
                                buffer[0] = 0;
                                buffer[1] = 0;
                                buffer[2] = 0;
                                buffer[3] = 0;
                                if(q565EncodedFile.read(buffer, 2) == 2) {
                                    rgb = Q565_Encoder::decodePixel(buffer[0], buffer[1]); // sent little endian (low end first)
                                    // write current pixel once,
                                    // check if color array is full

                                    statement.type = PIXEL_STATEMENT;
                                    statement.data = data;
                                    statement.q565Pixel = rgb;
                                    statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                    generatedStatements.append(statement);
                                    Q565_Encoder::encodeColorTable(rgb, colorTable);
                                    //Q565_Encoder::writePixel(result, Q565_Encoder::convertToQRgb(rgb), x, y);
                                } else {
                                    // error expected encoded RGB
                                    //return false;
                                }
                            } else if (data <=61) { // run found
                                data +=1; // run count has a -1 offset when sent
                                //qDebug() << "Decode: Found run of " << data << "of pixel ";
                                statement.type = RUN_STATEMENT;
                                statement.data = data;
                                statement.q565Pixel = rgb;
                                statement.fullPixel = Q565_Encoder::convertToQRgb(rgb);
                                generatedStatements.append(statement);
                                //Q565_Encoder::writePixel(result, Q565_Encoder::convertToQRgb(rgb), x, y);
                            }
                            break;
                        }
                        default:{
                            qDebug() << "Found incorrect type" << type;
                            break;
                        }
                    }
                }
            }
        }

        // generate the statementListModel

        return generatedStatements; // implicitly shared (Qlist)
    }
};
#endif // Q565_H
