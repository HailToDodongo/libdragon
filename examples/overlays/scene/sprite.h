#ifndef SPRITE_H
#define SPRITE_H

#include <libdragon.h>

class Sprite {
public:
    Sprite();
    ~Sprite();

public:
    void Draw();
    void SetPos(float x, float y);
    void SetScale(float x, float y);
    void SetAngle(float theta);
    void SetImage(const char *path);
    void SetImage(sprite_t *image);
    
private:
    void FreeImage();
    
private:
    sprite_t *m_image;
    bool m_image_owned;
    float m_pos_x, m_pos_y;
    float m_scale_x, m_scale_y;
    float m_angle;
};

#endif