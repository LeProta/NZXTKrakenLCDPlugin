#ifndef Q565_H
#define Q565_H

#include <QRgb>
#include <QDebug>
#include <QImage>
#include <QByteArray>

// =============================================================================
//  Encodeur Q565 (flux LCD du Kraken Elite V2).
//  Encodeur SEUL : le décodage est fait par le firmware du device. L'ancien
//  décodeur de diagnostic (generateStatementListModel & co) a été supprimé
//  (code mort). Fonctions `inline` : ce header est inclus par plusieurs TU.
// =============================================================================
namespace Q565_Encoder {
    constexpr unsigned short MAX_COLORS = 64;
    // Anti-artefacts : désactive les opcodes 2 octets (LUMA / INDEX_DIFF), suspectés
    // de désynchroniser le décodeur du firmware sur certaines frames (corruption qui
    // se propage → bandes / bloc en bas). RUN, INDEX et DIFF (1 octet) restent actifs.
    constexpr bool Q565_USE_COMPLEX_OPS = false;
    constexpr unsigned short Q565_OP_RGB565     = 0xFE;       // littéral 16 bits
    constexpr unsigned short Q565_OP_INDEX      = 0b00000000; // index table (implicite : hash brut)
    constexpr unsigned short Q565_OP_INDEX_DIFF = 0b10100000;
    constexpr unsigned short Q565_OP_DIFF       = 0b01000000;
    constexpr unsigned short Q565_OP_LUMA       = 0b10000000;
    constexpr unsigned short Q565_OP_RUN        = 0b11000000;
    constexpr unsigned short Q565_OP_END        = 0xFF;
    const int QRGB_ARRAY_SIZE = 1228809;
    constexpr char Q565_FORMAT_TAG[] = "q565";

    inline quint16 get565Pixel2(QRgb rgb){ // assumed 8,8,8 bit rgb
        auto red = (qRed(rgb) * 249 + 1014) >> 11 ;
        auto green = (qGreen(rgb) * 253 + 505) >> 10 ;
        auto blue = (qBlue(rgb) * 249 + 1014) >> 11;
        return  red << 11 | green << 5 | blue;
    }

    inline quint8 pixelHash(quint16 pixel) {
        unsigned short a = (pixel & 0b1111111100000000) >> 8;
        unsigned short b = pixel & 0b0000000011111111;
        return (a+b) & 0b111111; // % 64
    }

    // Table de couleurs a acces direct (le hash EST l'index, 0..63).
    // Remplace l'ancien QMap<quint8,quint16> : contains/value/insert par PIXEL
    // (409 600 pixels/frame a 60 fps) passaient par un arbre rouge-noir avec
    // allocations — le point chaud de tout l'encodeur.
    struct ColorTable {
        quint16 color[MAX_COLORS] = {};
        bool    used [MAX_COLORS] = {};
    };

    inline bool wouldEncodeColorTable(quint16 pixel, const ColorTable& colorTable, quint8& colorHash) {
        colorHash = pixelHash(pixel);
        if(!colorTable.used[colorHash]) {
            return true;   // slot libre -> insertion toujours possible (64 slots max)
        }
        return colorTable.color[colorHash] != pixel;
    }

    inline quint8 encodeColorTable(quint16 pixel, ColorTable& colorTable) {
        quint8 colorHash = pixelHash(pixel);
        colorTable.used [colorHash] = true;
        colorTable.color[colorHash] = pixel;   // insertion ou remplacement (collision)
        return colorHash;
    }

    inline void encodeFullPixel(QByteArray* encodeArray, quint16 currentPixel) {
        encodeArray->append(char(Q565_OP_RGB565));            // RGB565 (full pixel marker)
        encodeArray->append(char(currentPixel & 0xFF));       // 565 low byte
        encodeArray->append(char((currentPixel >> 8) & 0xFF)); // 565 high byte
    }

    inline qint8 calculateDiff(quint8 newValue, quint8 previousValue, quint8 max)
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
    }

    inline bool encodeDiff(QByteArray* encodeArray, quint16 r_new, quint16 g_new, quint16 b_new, quint16 previous) {
        quint8 r_prev, g_prev, b_prev;
        qint8 r_diff, g_diff, b_diff;
        r_prev = (previous >> 11) & 0b11111;
        g_prev = (previous >> 5)  & 0b111111;
        b_prev = previous         & 0b11111;

        r_diff = calculateDiff(r_new, r_prev , 32) + 2;
        g_diff = calculateDiff(g_new, g_prev, 64) + 2;
        b_diff = calculateDiff(b_new, b_prev, 32) + 2;
        // prefer diff over color table
        if ((r_diff <= 3 && r_diff >= 0) && (g_diff <= 3 && g_diff >= 0) && (b_diff <= 3 && b_diff >= 0) ){
            encodeArray->append(char(Q565_OP_DIFF | r_diff << 4 | g_diff << 2 | b_diff));
            return true;
        }
        return false;
    }

    inline bool encodeIndexDiffed(QByteArray* encodeArray, quint16 r, quint16 g, quint16 b, const ColorTable& colorTable)
    {
        qint8 r_diff, g_diff, b_diff;
        quint16 color;

        for (int k = 0; k < MAX_COLORS; ++k) {
            if(!colorTable.used[k]) continue;
            color = colorTable.color[k];
            r_diff = calculateDiff(r, (color >> 11), 32) + 2;
            g_diff = calculateDiff(g, ((color >> 5) & 0b111111), 64) + 4;
            b_diff = calculateDiff(b, color & 0b11111, 32) + 2;
            if(    r_diff <= 3 && r_diff >= 0
                && g_diff <= 7 && g_diff >= 0
                && b_diff <= 3 && b_diff >= 0) {
                quint8 byte1 = Q565_OP_INDEX_DIFF | (g_diff << 2) | r_diff;
                quint8 byte2 = (b_diff << 6) | quint8(k);
                encodeArray->append(char(byte1));
                encodeArray->append(char(byte2));
                return true;
            }
        }
        return false;
    }

    inline bool encodeLuma(QByteArray* encodeArray, quint16 previous, quint16 r, quint16 g, quint16 b)
    {
        quint8 r_prev, g_prev, b_prev;
        qint8 rg_diff, g_diff, bg_diff;
        r_prev = (previous >> 11) & 0b11111;
        g_prev = (previous >> 5)  & 0b111111;
        b_prev = previous         & 0b11111;
        g_diff = calculateDiff(g, g_prev, 64);
        rg_diff = calculateDiff(r, r_prev , 32) - g_diff;
        bg_diff = calculateDiff(b, b_prev, 32)  - g_diff;
        if ((rg_diff >= -8 && rg_diff <= 7) && (g_diff >= -16 && g_diff <= 15) && (bg_diff >= -8 && bg_diff <= 7) ){
            encodeArray->append(char(Q565_OP_LUMA | ((g_diff + 16) & 0b11111)));
            encodeArray->append(char(((rg_diff+8) << 4) | (bg_diff+ 8)));
            return true;
        }
        return false;
    }

    inline void encodePixel(QByteArray* encodeArray,  quint16 r, quint16 g, quint16 b, quint16 previous, quint16 current, ColorTable& colorTable)
    {
        quint8 colorHash;
        bool wouldAdd = wouldEncodeColorTable(current, colorTable, colorHash);
        if(!wouldAdd && colorHash < MAX_COLORS) {
            encodeArray->append(char(colorHash)); // 565_OP_INDEX
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

    inline QByteArray encode(const QImage& image, QByteArray* encodeArray = nullptr) {
        // Buffer local si l'appelant n'en fournit pas (pas de fuite) ; sinon
        // réutilisation du buffer préalloué (resize(0) garde la capacité).
        QByteArray localBuffer;
        if(encodeArray == nullptr) {
            localBuffer.reserve(QRGB_ARRAY_SIZE);
            encodeArray = &localBuffer;
        } else {
            encodeArray->resize(0);
        }
        const QImage& convertedImage = image;
        encodeArray->append(Q565_FORMAT_TAG); // write the magic header
        quint16 w16(convertedImage.width());
        quint16 h16(convertedImage.height());
        // encode in little endian
        encodeArray->append(char(w16 & 0xFF));
        encodeArray->append(char(w16 >> 8));
        encodeArray->append(char(h16 & 0xFF));
        encodeArray->append(char(h16 >> 8));
        quint16 previous16 = 0;
        quint8 r_new, g_new, b_new;
        quint16 currentPixel = 0;
        ColorTable colorTable;
        colorTable.used[0]  = true; colorTable.color[0]  = 0x0000;   // noir
        colorTable.used[63] = true; colorTable.color[63] = 0xFFFF;   // blanc
        unsigned char run{0};
        QRgb rgb{0};
        QRgb previousRgb{0};
        const QRgb* line = nullptr;
        for(int y = 0; y < convertedImage.height(); ++y) {
            line = reinterpret_cast<const QRgb*>(convertedImage.constScanLine(y));
            for(int x = 0; x < convertedImage.width(); ++x) {
                rgb = line[x];

                if((previousRgb == rgb) && ( y > 0 || x > 0) ) {
                    if( run > 61) { // 62 répétitions en attente : flush (max opcode = 61 → 62 pixels)
                        run -= 1;
                        encodeArray->append(char(Q565_OP_RUN | run));
                        run = 1;
                    } else {
                        ++run;
                        continue;
                    }
                } else {
                    currentPixel = get565Pixel2(rgb);
                    if(run >= 1) { // run en cours : flush
                        run -= 1; // le décodeur ajoute +1 (offset -1 à l'envoi)
                        encodeArray->append(char(Q565_OP_RUN | run));
                        run = 0;
                    }
                    r_new = (currentPixel >> 11) & 0b11111;
                    g_new = (currentPixel >> 5)  & 0b111111;
                    b_new = currentPixel         & 0b11111;
                    // prefer diff over color table
                    if (!encodeDiff(encodeArray, r_new, g_new, b_new, previous16)){
                        encodePixel(encodeArray, r_new, g_new, b_new, previous16, currentPixel, colorTable);
                    }
                }
                previous16 = currentPixel;
                previousRgb = rgb;
            }
        }
        // Flush du run final. `>= 1` (et pas `> 1`) : run == 1 correspond à UN
        // pixel répété en attente ; l'ancien `> 1` le perdait → le décodeur du
        // firmware recevait width*height-1 pixels (décalage en fin de frame).
        if(run >= 1) {
            --run;
            encodeArray->append(char(Q565_OP_RUN | run));
        }
        encodeArray->append(char(Q565_OP_END));
        return (*encodeArray);
    }
};
#endif // Q565_H
