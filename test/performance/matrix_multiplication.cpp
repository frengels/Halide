#include "Halide.h"
#include "halide_benchmark.h"
#include <cstdio>

using namespace Halide;
using namespace Halide::Tools;
using namespace Halide::ConciseCasts;

void simple_version(float16_t *A, float16_t *B, float *C, int width, int stride) {
    for (int iy = 0; iy < width; iy++) {
        for (int ix = 0; ix < width; ix++) {
            float *cc = C + iy * stride + ix;
            *cc = 0.0f;

            for (int ik = 0; ik < width; ik++) {
                *cc = *cc + static_cast<float>(A[iy * stride + ik]) * static_cast<float>(B[ik * stride + ix]);
            }
        }
    }
}

int main(int argc, char **argv) {
    Target target = get_jit_target_from_environment();
    if (target.arch == Target::WebAssembly) {
        printf("[SKIP] Performance tests are meaningless and/or misleading under WebAssembly interpreter.\n");
        return 0;
    }

    const int matrix_size = 992;

    ImageParam A(type_of<float16_t>(), 2);
    ImageParam B(type_of<float16_t>(), 2);

    Var x("x"), xi("xi"), xo("xo"), y("y"), yo("yo"), yi("yi"), yii("yii"), xii("xii");
    Func matrix_mul("matrix_mul");

    RDom k(0, matrix_size);
    RVar ki;

    matrix_mul(x, y) += f32(A(k, y) * B(x, k));

    Func out;

    if (target.has_feature(Target::CUDA)) {
        out = matrix_mul;

        Var blockX("blockX"), blockY("blockY"), threadX("threadX"), threadY("threadY");
        out
            .update()
            .gpu_tile(x, y, blockX, blockY, threadX, threadY, 16, 16);
    } else {
        out(x, y) = matrix_mul(x, y);

        Var xy;

        out.tile(x, y, xi, yi, 24, 32)
            .fuse(x, y, xy)
            .parallel(xy)
            .split(yi, yi, yii, 4)
            .vectorize(xi, 8)
            .unroll(xi)
            .unroll(yii);

        matrix_mul.compute_at(out, yi)
            .vectorize(x, 8)
            .unroll(y);

        matrix_mul.update(0)
            .reorder(x, y, k)
            .vectorize(x, 8)
            .unroll(x)
            .unroll(y)
            .unroll(k, 2);

        out
            .bound(x, 0, matrix_size)
            .bound(y, 0, matrix_size);
    }

    out.compile_jit();

    Buffer<float16_t> mat_A(matrix_size, matrix_size);
    Buffer<float16_t> mat_B(matrix_size, matrix_size);
    Buffer<float> output(matrix_size, matrix_size);

    // init randomly
    for (int iy = 0; iy < matrix_size; iy++) {
        for (int ix = 0; ix < matrix_size; ix++) {
            mat_A(ix, iy) = static_cast<float16_t>((rand() % 256) / 256.0f);
            mat_B(ix, iy) = static_cast<float16_t>((rand() % 256) / 256.0f);
        }
    }

    A.set(mat_A);
    B.set(mat_B);

    out.realize(output);

    double t = benchmark([&]() {
        out.realize(output);
    });

    // check results
    Buffer<float> output_ref(matrix_size, matrix_size);
    Buffer<float> output_halide(matrix_size, matrix_size);

    simple_version(mat_A.data(), mat_B.data(), output_ref.data(), mat_A.width(), mat_A.stride(1));
    output_halide = out.realize({matrix_size, matrix_size});

    bool halide_correct = true;
    for (int iy = 0; iy < matrix_size && halide_correct; iy++) {
        for (int ix = 0; ix < matrix_size; ix++) {
            halide_correct = halide_correct && (std::abs(output_ref(ix, iy) - output_halide(ix, iy)) < 0.001f);
        }
    }

    if (halide_correct) {
        printf("Halide results - OK\n");
    } else {
        printf("Halide results - FAIL\n");
        return 1;
    }

    // Uncomment to see the generated assembly.
    /*
    {
        Target t("host-no_asserts-no_runtime-no_bounds_query");
        out.compile_to_assembly("/dev/stdout", matrix_mul.infer_arguments(), t);
    }
    */

    float gflops = 2.0f * matrix_size * matrix_size * matrix_size / 1e9f;

    printf("Halide: %fms, %f GFLOP/s\n\n", t * 1e3, (gflops / t));

    printf("Success!\n");
    return 0;
}
