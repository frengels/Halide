#include "common_halide.h"

using namespace Halide;
using namespace Halide::ConciseCasts;

namespace hannk {

int get_register_count(const Target &target) {
    switch (target.arch) {
        case Target::X86:
            return target.has_feature(Target::AVX512_Skylake) ? 32 : 16;
        case Target::ARM:
            return target.bits == 64 ? 32 : 16;
        case Target::Hexagon:
            return 32;
        default:
            return 16;
    }
}

void interpret_as_tensor(OutputImageParam p) {
    p.dim(0).set_stride(1).set_min(0);
}

void require_same_min_extent(int first_dim, OutputImageParam first, int second_dim, OutputImageParam second) {
    second.dim(second_dim).set_min(first.dim(first_dim).min());
    second.dim(second_dim).set_extent(first.dim(first_dim).extent());
}

void require_same_min_extent(int d, OutputImageParam first, OutputImageParam second) {
    second.dim(d).set_min(first.dim(d).min());
    second.dim(d).set_extent(first.dim(d).extent());
}

Expr is_interleaved(OutputImageParam p, int channels) {
    return
        p.dim(0).min() == 0 &&
        p.dim(0).extent() == channels &&
        p.dim(1).stride() == channels;
}

Func constant_exterior_tensor(
    Func t, Expr exterior,
    Expr min_c, Expr extent_c,
    Expr min_x, Expr extent_x,
    Expr min_y, Expr extent_y,
    Expr min_b, Expr extent_b) {
    Var c("c"), x("x"), y("y"), b("b");
    // We usually don't care about what comes after the boundary in the c
    // or b dimensions, so just skip those for the select.
    Expr in_bounds =
        min_x <= x && x < min_x + extent_x &&
        min_y <= y && y < min_y + extent_y;
    Expr bounded = t(clamp(c, min_c, min_c + extent_c - 1),
                     clamp(x, min_x, min_x + extent_x - 1),
                     clamp(y, min_y, min_y + extent_y - 1),
                     clamp(b, min_b, min_b + extent_b - 1));

    Func tensor_bounded("tensor_bounded");
    tensor_bounded(c, x, y, b) = select(in_bounds, bounded, exterior);

    return tensor_bounded;
}

Func constant_exterior_tensor(ImageParam p, Expr exterior) {
    return constant_exterior_tensor(p, exterior,
                                    p.dim(0).min(), p.dim(0).extent(),
                                    p.dim(1).min(), p.dim(1).extent(),
                                    p.dim(2).min(), p.dim(2).extent(),
                                    p.dim(3).min(), p.dim(3).extent());
}

Expr align_down(const Expr &x, const Expr &n) {
    return (x / n) * n;
}

Expr align_up(const Expr &x, const Expr &n) {
    return ((x + n - 1) / n) * n;
}

Expr align(const Expr &x, const Expr &n) {
    return align_down(x, n);
}

Expr multiply_2x_high(const Expr &a, const Expr &b) {
    Type t = a.type().bits() > b.type().bits() ? a.type() : b.type();
    Expr ab_wide = widening_mul(a, b);
    // In Halide, integer division rounds to negative infinity, so division by a
    // power of two is the same as a shift (unlike C).
    // TODO: Using rounding_shift_right here doesn't generate qrdmulh :(
    int nudge = 1 << (t.bits() - 2);
    Expr result = (ab_wide + nudge) >> (t.bits() - 1);
    return saturating_cast(t, result);
}

Expr multiply_quantized(const Expr &x, const Expr &q, const Expr &shift) {
    return rounding_shift_right(multiply_2x_high(x, q), shift);
}

Expr approx_log2(const Expr &x, int log2_precision) {
    int precision = 1 << log2_precision;

    //   floor(log2(x)) = B - clz(x) => log2(x) ~ B - clz(x)
    //   B = sizeof(x)*8 - 1
    //   clz(x) = count_leading_zeros(x)
    int log2_max_x = x.type().bits() - 1;
    Expr floor_log2 = log2_max_x - i16(count_leading_zeros(x));

    // Use the bits after the leading bit to linearly interpolate to the next
    // power of 2. In other words, we want the slope of the line between
    // floor(log2(x)) and floor(log2(x)) + 1.
    Expr frac =
        cast<int>((x >> (floor_log2 - log2_precision)) % precision);

    // For x <= 0, return any negative value. If count_leading_zeros returns
    // x.type().bits(), which appears to be the case on every platform we
    // target, both sides of this select are the same (if log2_precision = 0).
    return select(x > 0, precision * cast<int>(floor_log2) + frac,
                  cast<int>(-1));
}

Expr approx_exp2(const Expr &x, const Expr &log2_precision_x, int log2_precision_result) {
    Expr precision_x = 1 << log2_precision_x;

    // Compute floor(x / precision_x) and frac(x / precision_x)
    Expr floor_x = clamp(x >> log2_precision_x, -31, 31);
    Expr frac_x = x - (floor_x << log2_precision_x);

    // Compute 2^floor(x / precision_x)*precision_result
    Expr exp_floor_x = (1 << log2_precision_result) << floor_x;

    // Linearly interpolate to the next power of 2 using frac_x.
    return exp_floor_x + multiply_2x_high(exp_floor_x, frac_x << (31 - log2_precision_x));
}

}  // namespace hannk
