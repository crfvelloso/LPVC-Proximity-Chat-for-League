#include "minimap_reader.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

MinimapReader::MinimapReader() {
    kernel_ = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
}

MinimapReader::~MinimapReader() {
    ReleaseGdi();
}

void MinimapReader::GetScreenSize(int& w, int& h) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0) w = 1920;
    if (h <= 0) h = 1080;
}

// O minimapa do LoL fica no canto inferior direito e sua altura acompanha a
// resolucao (~26% da altura da tela na escala padrao da interface).
MinimapRegion MinimapReader::AutoDetectRegion() {
    int sw, sh;
    GetScreenSize(sw, sh);
    MinimapRegion r;
    r.w = r.h = (int)(sh * 0.26f);
    r.x = sw - r.w;
    r.y = sh - r.h;
    return r;
}

void MinimapReader::Configure(const MinimapRegion& region) {
    MinimapRegion r = region;
    int sw, sh;
    GetScreenSize(sw, sh);
    r.w = std::clamp(r.w, 64, sw);
    r.h = std::clamp(r.h, 64, sh);
    r.x = std::clamp(r.x, 0, sw - r.w);
    r.y = std::clamp(r.y, 0, sh - r.h);

    if (r.x == region_.x && r.y == region_.y && r.w == region_.w && r.h == region_.h && bitmap_)
        return;

    region_ = r;
    ReleaseGdi();          // recria os recursos GDI no tamanho novo
    lastX_ = lastY_ = -1.0f;
    missCount_ = 0;
}

void MinimapReader::ReleaseGdi() {
    if (bitmap_)   { DeleteObject(bitmap_); bitmap_ = nullptr; }
    if (memDc_)    { DeleteDC(memDc_); memDc_ = nullptr; }
    if (screenDc_) { ReleaseDC(nullptr, screenDc_); screenDc_ = nullptr; }
}

// Os objetos GDI sao criados uma unica vez, nao a cada quadro: era isso que
// fazia a captura custar caro no loop antigo.
bool MinimapReader::EnsureGdi() {
    if (bitmap_ && memDc_ && screenDc_) return true;
    if (region_.w <= 0 || region_.h <= 0) return false;

    ReleaseGdi();
    screenDc_ = GetDC(nullptr);
    if (!screenDc_) return false;
    memDc_ = CreateCompatibleDC(screenDc_);
    if (!memDc_) { ReleaseGdi(); return false; }
    SetStretchBltMode(memDc_, COLORONCOLOR);

    bitmap_ = CreateCompatibleBitmap(screenDc_, region_.w, region_.h);
    if (!bitmap_) { ReleaseGdi(); return false; }
    SelectObject(memDc_, bitmap_);

    bmpInfo_ = {};
    bmpInfo_.biSize = sizeof(BITMAPINFOHEADER);
    bmpInfo_.biWidth = region_.w;
    bmpInfo_.biHeight = -region_.h;    // negativo = origem no topo
    bmpInfo_.biPlanes = 1;
    bmpInfo_.biBitCount = 32;
    bmpInfo_.biCompression = BI_RGB;

    frameBgra_.create(region_.h, region_.w, CV_8UC4);
    return true;
}

bool MinimapReader::Capture() {
    if (!EnsureGdi()) return false;
    if (!BitBlt(memDc_, 0, 0, region_.w, region_.h, screenDc_, region_.x, region_.y, SRCCOPY)) {
        ReleaseGdi();      // a sessao pode ter mudado (bloqueio de tela, RDP)
        return false;
    }
    return GetDIBits(memDc_, bitmap_, 0, region_.h, frameBgra_.data,
                     (BITMAPINFO*)&bmpInfo_, DIB_RGB_COLORS) != 0;
}

MapPos MinimapReader::FindSelf() {
    MapPos pos;
    candidates_ = 0;
    if (frameBgra_.empty()) return pos;

    // O que rastreamos e o retangulo branco da camera desenhado no minimapa
    // (ele acompanha o campeao). Como e branco puro, da para procurar direto no
    // BGRA, sem a conversao para HSV que o codigo antigo fazia a cada quadro.
    const int b = brightness_;
    cv::inRange(frameBgra_, cv::Scalar(b, b, b, 0), cv::Scalar(255, 255, 255, 255), mask_);
    cv::dilate(mask_, mask_, kernel_, cv::Point(-1, -1), 2);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask_, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    float bestScore = -1.0f;
    int bestX = 0, bestY = 0;

    for (const auto& contour : contours) {
        const cv::Rect box = cv::boundingRect(contour);
        if (box.width < minBox_ || box.width > maxBox_) continue;
        if (box.height < minBox_ * 3 / 4 || box.height > maxBox_) continue;

        // O retangulo da camera e deitado (proporcao da tela); texto e barra de
        // vida sao muito mais alongados e caem fora dessa faixa.
        const float aspect = (float)box.width / (float)std::max(1, box.height);
        if (aspect < 0.7f || aspect > 2.6f) continue;

        candidates_++;
        const float cx = box.x + box.width * 0.5f;
        const float cy = box.y + box.height * 0.5f;

        // Pontuacao: area (o maior candidato costuma ser a camera) mais um bonus
        // por estar perto de onde estavamos no quadro anterior.
        float score = (float)(box.width * box.height) / (float)(region_.w * region_.h);
        if (lastX_ >= 0.0f) {
            const float dx = cx - lastX_, dy = cy - lastY_;
            const float dist = std::sqrt(dx * dx + dy * dy);
            score += std::max(0.0f, 1.0f - dist / (region_.w * 0.25f));
        }
        if (score > bestScore) {
            bestScore = score;
            bestX = (int)cx;
            bestY = (int)cy;
        }
    }

    if (bestScore < 0.0f) {
        // Tolera algumas falhas seguidas antes de declarar posicao perdida
        // (pings e animacoes cobrem o retangulo por um ou dois quadros).
        if (++missCount_ > 8) lastX_ = lastY_ = -1.0f;
        return pos;
    }

    missCount_ = 0;
    // Suavizacao leve, para o volume nao oscilar com o tremor da deteccao.
    if (lastX_ >= 0.0f) {
        lastX_ = lastX_ * 0.4f + bestX * 0.6f;
        lastY_ = lastY_ * 0.4f + bestY * 0.6f;
    } else {
        lastX_ = (float)bestX;
        lastY_ = (float)bestY;
    }

    pos.x = std::clamp(lastX_ / (float)region_.w, 0.0f, 1.0f);
    pos.y = std::clamp(lastY_ / (float)region_.h, 0.0f, 1.0f);
    pos.is_valid = true;
    return pos;
}

bool MinimapReader::GetPreviewRGBA(std::vector<uint8_t>& out, int& w, int& h) const {
    if (frameBgra_.empty()) return false;
    w = frameBgra_.cols;
    h = frameBgra_.rows;
    out.resize((size_t)w * h * 4);
    const uint8_t* src = frameBgra_.data;
    uint8_t* dst = out.data();
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {            // BGRA -> RGBA
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = 255;
    }
    return true;
}
