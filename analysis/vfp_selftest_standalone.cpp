/* Standalone VFP self-test for libMassEffect.so, extracted verbatim from
 * src/vfp_vector_patch.cpp. Runs under qemu-arm without the loader. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define warning(...) fprintf(stderr, __VA_ARGS__)
#define trace(...)   do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

enum class VfpOp {
    Move,
    Add,
    Subtract,
    Multiply,
    MultiplyAccumulate,
};

struct DecodedVfp {
    VfpOp op;
    uint8_t condition;
    uint8_t destination;
    uint8_t left;
    uint8_t right;
    bool uses_left;
};

struct VectorPatch {
    uint32_t offset;
    uint32_t expected;
    uint8_t lanes;
    uint8_t stride;
};

static const VectorPatch kVectorPatches[] = {
    {0x005828ec, 0xee288a04, 2, 2},   /* vmul.f32	s16, s16, s8 */
    {0x005828f0, 0xee2aaa04, 2, 2},   /* vmul.f32	s20, s20, s8 */
    {0x00582904, 0xee0c8a06, 2, 2},   /* vmla.f32	s16, s24, s12 */
    {0x00582918, 0xee0e8a06, 2, 2},   /* vmla.f32	s16, s28, s12 */
    {0x0058292c, 0xeeb06a48, 2, 2},   /* vmov.f32	s12, s16 */
    {0x00582930, 0xee0caa06, 2, 2},   /* vmla.f32	s20, s24, s12 */
    {0x00582944, 0xee0eaa06, 2, 2},   /* vmla.f32	s20, s28, s12 */
    {0x00587194, 0xee24ca00, 6, 1},   /* vmul.f32	s24, s8, s0 */
    {0x0058719c, 0xee3cca08, 6, 1},   /* vadd.f32	s24, s24, s16 */
    {0x005871a8, 0xee3cca20, 6, 1},   /* vadd.f32	s24, s24, s1 */
    {0x005887fc, 0xee244a00, 6, 1},   /* vmul.f32	s8, s8, s0 */
    {0x00588800, 0xee388a4c, 6, 1},   /* vsub.f32	s16, s16, s24 */
    {0x00588804, 0xee04ca08, 6, 1},   /* vmla.f32	s24, s8, s16 */
    {0x00588a68, 0x7e388a04, 6, 1},   /* vaddvc.f32	s16, s16, s8 */
    {0x00588adc, 0x7e048a00, 6, 1},   /* vmlavc.f32	s16, s8, s0 */
    {0x0058f8f0, 0xee28ca00, 6, 1},   /* vmul.f32	s24, s16, s0 */
    {0x0058f8f8, 0xee3cca20, 6, 1},   /* vadd.f32	s24, s24, s1 */
    {0x0058f8fc, 0xee34ca4c, 6, 1},   /* vsub.f32	s24, s8, s24 */
    {0x0058f904, 0xee2cca00, 6, 1},   /* vmul.f32	s24, s24, s0 */
    {0x0058f908, 0xee3cca08, 6, 1},   /* vadd.f32	s24, s24, s16 */
    {0x0058f90c, 0xee2c4a01, 6, 1},   /* vmul.f32	s8, s24, s2 */
    {0x0058f960, 0xee28ca00, 6, 1},   /* vmul.f32	s24, s16, s0 */
    {0x0058f968, 0xee3cca20, 6, 1},   /* vadd.f32	s24, s24, s1 */
    {0x0058f96c, 0xee34ca4c, 6, 1},   /* vsub.f32	s24, s8, s24 */
    {0x0058f974, 0xee2cca00, 6, 1},   /* vmul.f32	s24, s24, s0 */
    {0x0058f97c, 0xee3cca08, 6, 1},   /* vadd.f32	s24, s24, s16 */
    {0x0058f980, 0xee0c4a01, 6, 1},   /* vmla.f32	s8, s24, s2 */
    {0x00591acc, 0x7e244a00, 6, 1},   /* vmulvc.f32	s8, s8, s0 */
    {0x00591ad0, 0x7e288a00, 6, 1},   /* vmulvc.f32	s16, s16, s0 */
};

static bool decode_f32(uint32_t raw, DecodedVfp *decoded)
{
    /* VFP data-processing encoding, single precision only. */
    if ((raw & 0x0f000e10) != 0x0e000a00 || (raw & 0x00000100))
        return false;

    int opc1 = (raw & 0x00b00000) >> 20;
    int opc2 = (raw & 0x000f0000) >> 16;
    int opc3 = (raw & 0x00000040) >> 6;
    decoded->uses_left = true;

    switch (opc1) {
    case 0b0000:
        if (opc3)
            return false;
        decoded->op = VfpOp::MultiplyAccumulate;
        break;
    case 0b0010:
        if (opc3)
            return false;
        decoded->op = VfpOp::Multiply;
        break;
    case 0b0011:
        decoded->op = opc3 ? VfpOp::Subtract : VfpOp::Add;
        break;
    case 0b1011:
        if (!opc3)
            return false; /* VMOV immediate is not used here. */
        opc3 = (raw & 0x000000c0) >> 6;
        if (opc2 != 0 || opc3 != 0b01)
            return false;
        decoded->op = VfpOp::Move;
        decoded->uses_left = false;
        break;
    default:
        return false;
    }

    decoded->condition = raw >> 28;
    decoded->destination =
        ((raw & 0x0000f000) >> 11) | ((raw & 0x00400000) >> 22);
    decoded->left =
        ((raw & 0x000f0000) >> 15) | ((raw & 0x00000080) >> 7);
    decoded->right =
        ((raw & 0x0000000f) << 1) | ((raw & 0x00000020) >> 5);
    return true;
}

static uint32_t encode_f32(const DecodedVfp &decoded,
                           uint32_t destination, uint32_t left,
                           uint32_t right)
{
    uint32_t instruction = 0;
    switch (decoded.op) {
    case VfpOp::Move:               instruction = 0xeeb00a40; break;
    case VfpOp::Add:                instruction = 0xee300a00; break;
    case VfpOp::Subtract:           instruction = 0xee300a40; break;
    case VfpOp::Multiply:           instruction = 0xee200a00; break;
    case VfpOp::MultiplyAccumulate: instruction = 0xee000a00; break;
    }

    instruction |= ((destination & 0x1e) << 11) |
                   ((destination & 0x01) << 22);
    if (decoded.uses_left) {
        instruction |= ((left & 0x1e) << 15) |
                       ((left & 0x01) << 7);
    }
    instruction |= ((right & 0x1e) >> 1) |
                   ((right & 0x01) << 5);
    return instruction;
}

static void generate_lanes(const DecodedVfp &decoded, unsigned int lanes,
                           unsigned int stride, uint32_t *out)
{
    /* Advance one operand by STRIDE for the next lane, wrapping inside its own
     * bank of eight. An operand in bank 0 is a scalar and does not advance -
     * that is how the second source stays fixed across a vector multiply. Dead
     * Space was entirely STRIDE=1 and could hard-code the step; Mass Effect's
     * Xas1 decoder has a STRIDE=2 region, so the step is a parameter now and
     * the same rule covers destination, left and right uniformly. */
    auto next = [stride](int reg) {
        int bank = reg & 0x18;
        if (bank == 0)          /* scalar: fixed */
            return reg;
        return bank | ((reg + (int)stride) & 0x7);
    };

    int destination = decoded.destination;
    int left = decoded.left;
    int right = decoded.right;

    for (unsigned int lane = 0; lane < lanes; lane++) {
        *out++ = encode_f32(decoded, destination, left, right);
        destination = next(destination);
        left = next(left);
        right = next(right);
    }
}

static const uint32_t kStubHead[] = {
    0xe92d4030,
    0xec900a20,
    0xeef14a10,
};

static const uint32_t kStubTail[] = {
    0xeee14a10,
    0xec810a20,
    0xe8bd8030,
};

typedef void (*StubFn)(const uint32_t *in, uint32_t *out);

static bool run_stub(void *page, uint32_t configure,
                     const uint32_t *body, unsigned int body_words,
                     const uint32_t *in, uint32_t *out)
{
    uint32_t *code = (uint32_t *)page;
    unsigned int n = 0;

    for (unsigned int i = 0; i < sizeof(kStubHead) / sizeof(kStubHead[0]); i++)
        code[n++] = kStubHead[i];
    code[n++] = configure;
    code[n++] = 0xeee15a10;                 /* vmsr fpscr, r5 */
    for (unsigned int i = 0; i < body_words; i++)
        code[n++] = body[i];
    for (unsigned int i = 0; i < sizeof(kStubTail) / sizeof(kStubTail[0]); i++)
        code[n++] = kStubTail[i];

    __builtin___clear_cache((char *)code, (char *)(code + n));
    ((StubFn)(void *)code)(in, out);
    return true;
}

static unsigned int selftest_vfp_expansion(void)
{
    /* Two pages: one holds the stub being assembled, and it is rewritten for
     * every case rather than kept around. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        warning("VFP self-test: no executable scratch page\n");
        return 0;
    }

    /* Distinct, exactly representable, mixed-sign inputs. Distinct matters:
     * identical register contents would hide a lane that reads the wrong
     * register, which is the most likely mistake in a hand-decoded list. */
    uint32_t input[32];
    for (int i = 0; i < 32; i++) {
        float v = (float)(i + 1) * ((i & 1) ? -0.25f : 0.5f);
        memcpy(&input[i], &v, sizeof(v));
    }

    unsigned int agreed = 0, disagreed = 0, vector_capable = 0;

    for (const VectorPatch &patch : kVectorPatches) {
        DecodedVfp decoded = {};
        if (!decode_f32(patch.expected, &decoded))
            continue;

        uint32_t lanes[8] = {};
        generate_lanes(decoded, patch.lanes, patch.stride, lanes);

        /* The condition code is carried by the branch that reaches the
         * trampoline, so the arithmetic itself is compared unconditionally. */
        uint32_t original = (patch.expected & 0x0fffffff) | 0xe0000000;

        /* Reference FPSCR carries both LEN (18:16) and STRIDE (21:20); Mass
         * Effect has a STRIDE=2 region, so setting LEN alone would compare the
         * expansion against the wrong reference. The control value fits one
         * rotated immediate - orr r5,r4,#ctrl with rot=8, low byte = ctrl>>16 -
         * and bic clears the same four bits. */
        uint32_t ctrl = ((uint32_t)(patch.lanes - 1) << 16) |
                        ((uint32_t)(patch.stride - 1) << 20);
        uint32_t set_len = 0xe3845800 | (ctrl >> 16);   /* orr r5,r4,#ctrl */
        uint32_t clear   = 0xe3c45837;             /* bic r5,r4,#0x370000 */

        uint32_t vector_out[32], scalar_out[32], single_out[32];
        run_stub(page, set_len, &original, 1, input, vector_out);
        run_stub(page, clear, lanes, patch.lanes, input, scalar_out);
        run_stub(page, clear, &original, 1, input, single_out);

        /* Does this host implement short vectors at all? If the vector run and
         * the deliberately-scalar run of the *same* opcode agree, LEN was
         * ignored - and then a mismatch below would say nothing about our
         * expansion. Reported rather than assumed. */
        if (memcmp(vector_out, single_out, sizeof(vector_out)) != 0)
            vector_capable++;

        if (memcmp(vector_out, scalar_out, sizeof(vector_out)) == 0) {
            agreed++;
            continue;
        }

        disagreed++;
        for (int i = 0; i < 32; i++) {
            if (vector_out[i] == scalar_out[i])
                continue;
            warning("VFP self-test +0x%08x (%08x, %u lanes): s%d vector=%08x "
                    "expansion=%08x\n", patch.offset, patch.expected,
                    patch.lanes, i, vector_out[i], scalar_out[i]);
        }
    }

    munmap(page, 4096);

    if (!vector_capable) {
        warning("VFP self-test: this host ignores FPSCR LEN, so the reference "
                "side of the comparison is not a vector - result is not "
                "meaningful here\n");
        return 0;
    }

    trace("VFP self-test: %u/%u expansions reproduce short-vector arithmetic "
          "exactly (%u disagreed, %u opcodes confirmed vectorising on this "
          "host)", agreed, agreed + disagreed, disagreed, vector_capable);
    return disagreed;
}

int main(void){ return selftest_vfp_expansion()==0 ? 0 : 1; }
