#ifndef MINIMAP_READER_H
#define MINIMAP_READER_H

#include <windows.h>
#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>

struct MapPos {
    float x = 0.0f;    // normalizado 0..1 dentro da regiao do minimapa
    float y = 0.0f;
    bool is_valid = false;
    bool valid() const { return is_valid; }
};

struct MinimapRegion {
    int x = 0, y = 0, w = 0, h = 0;
};

class MinimapReader {
public:
    MinimapReader();
    ~MinimapReader();
    MinimapReader(const MinimapReader&) = delete;
    MinimapReader& operator=(const MinimapReader&) = delete;

    // Estimativa da regiao do minimapa a partir da resolucao da tela.
    static MinimapRegion AutoDetectRegion();
    static void GetScreenSize(int& w, int& h);

    void Configure(const MinimapRegion& region);
    MinimapRegion GetRegion() const { return region_; }

    bool Capture();
    MapPos FindSelf();

    void SetBrightness(int value) { brightness_ = value; }   // 0..255
    // Tamanho (em pixels) do retangulo branco da camera dentro do minimapa.
    void SetBoxSize(int minPx, int maxPx) { minBox_ = minPx; maxBox_ = maxPx; }
    int  GetBrightness() const { return brightness_; }
    int  GetCandidateCount() const { return candidates_; }

    // Copia a ultima captura em RGBA (para a previa de calibracao na UI).
    bool GetPreviewRGBA(std::vector<uint8_t>& out, int& w, int& h) const;

private:
    void ReleaseGdi();
    bool EnsureGdi();

    MinimapRegion region_;
    cv::Mat frameBgra_;   // captura crua (BGRA)
    cv::Mat mask_;
    cv::Mat kernel_;

    HDC screenDc_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    BITMAPINFOHEADER bmpInfo_{};

    int brightness_ = 200;
    int minBox_ = 30;
    int maxBox_ = 250;
    int candidates_ = 0;

    // Continuidade temporal: entre varios candidatos, prefere o mais proximo
    // da ultima posicao valida (a camera nao pula de um lado ao outro do mapa).
    float lastX_ = -1.0f, lastY_ = -1.0f;
    int missCount_ = 0;
};

#endif
