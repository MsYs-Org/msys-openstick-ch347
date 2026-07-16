#ifndef MSYS_TOUCH_AFFINE_H
#define MSYS_TOUCH_AFFINE_H

#include <math.h>

struct touch_affine {
    double value[9];
    unsigned int revision;
};

static inline void touch_affine_identity(struct touch_affine *affine)
{
    static const double identity[9] = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };

    for (unsigned int i = 0; i < 9; i++)
        affine->value[i] = identity[i];
    affine->revision = 0;
}

static inline int touch_affine_valid(const struct touch_affine *affine)
{
    double determinant;

    if (!affine)
        return 0;
    for (unsigned int i = 0; i < 9; i++) {
        if (!isfinite(affine->value[i]) || fabs(affine->value[i]) > 4.0)
            return 0;
    }
    if (fabs(affine->value[6]) > 1e-9 ||
            fabs(affine->value[7]) > 1e-9 ||
            fabs(affine->value[8] - 1.0) > 1e-9)
        return 0;
    determinant = affine->value[0] * affine->value[4] -
        affine->value[1] * affine->value[3];
    if (fabs(determinant) < 1e-6)
        return 0;

    for (unsigned int y = 0; y <= 1; y++) {
        for (unsigned int x = 0; x <= 1; x++) {
            double mapped_x = affine->value[0] * x +
                affine->value[1] * y + affine->value[2];
            double mapped_y = affine->value[3] * x +
                affine->value[4] * y + affine->value[5];

            if (mapped_x < -1.0 || mapped_x > 2.0 ||
                    mapped_y < -1.0 || mapped_y > 2.0)
                return 0;
        }
    }
    return 1;
}

static inline void touch_affine_apply(const struct touch_affine *affine,
        double x, double y, double *mapped_x, double *mapped_y)
{
    *mapped_x = affine->value[0] * x + affine->value[1] * y +
        affine->value[2];
    *mapped_y = affine->value[3] * x + affine->value[4] * y +
        affine->value[5];
}

#endif
