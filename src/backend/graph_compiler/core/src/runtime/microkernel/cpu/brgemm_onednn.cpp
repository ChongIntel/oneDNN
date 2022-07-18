/*******************************************************************************
 * Copyright 2020-2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include <cstring>
#include <dnnl.h>
#include <iostream>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include "brgemm_common.hpp"
#include "kernel_timer.hpp"
#include "microkernel.hpp"
#include <cpu/x64/amx_tile_configure.hpp>
#include <cpu/x64/brgemm/brgemm.hpp>
#include <cpu/x64/brgemm/brgemm_types.hpp>
#include <runtime/config.hpp>
#include <runtime/context.hpp>
#include <runtime/data_type.hpp>
#include <runtime/kernel_include/x86simd/vec_u32x4.hpp>
#include <runtime/os.hpp>
#include <runtime/thread_locals.hpp>
#include <unordered_map>
#include <util/hash_utils.hpp>
#include <util/os.hpp>

using namespace dnnl::impl::cpu::x64;
using namespace sc::brgemm;
typedef sc::sc_data_etype sc_dtype;

static dnnl_data_type_t convert_dnnl_dtype(int dtype) {
    switch (sc_dtype(dtype)) {
        case sc_dtype::F32: return dnnl_f32;
        case sc_dtype::S32: return dnnl_s32;
        case sc_dtype::BF16: return dnnl_bf16;
        case sc_dtype::S8: return dnnl_s8;
        case sc_dtype::U8: return dnnl_u8;
        default:
            throw std::runtime_error(
                    "convert_dnnl_dtype error, currently only support datatype "
                    "f32/s32/bf16/s8/u8");
    }
}

static size_t get_dtype_sizeof(int dtype) {
    switch (sc_dtype(dtype)) {
        case sc_dtype::F32: return sizeof(float);
        case sc_dtype::S32: return sizeof(int32_t);
        case sc_dtype::BF16: return sizeof(uint16_t);
        case sc_dtype::S8: return sizeof(int8_t);
        case sc_dtype::U8: return sizeof(uint8_t);
        default:
            throw std::runtime_error(
                    "Get dtype size error, currently only support datatype "
                    "f32/s32/bf16/s8/u8");
    }
}

static brgemm_attr_t get_dnnl_brgemm_attrs(const attrs_setting_t &attrs) {
    brgemm_attr_t dnnl_attrs;
    for (int i = 0; i < attrs.num_; i++) {
        std::pair<attr_key, int64_t> it = attrs.map_[i];
        switch (it.first) {
            case attr_key::max_bs:
                dnnl_attrs.max_bs = static_cast<int>(it.second);
                break;
            case attr_key::max_top_vpad:
                dnnl_attrs.max_top_vpad = static_cast<int>(it.second);
                break;
            case attr_key::max_bottom_vpad:
                dnnl_attrs.max_bottom_vpad = static_cast<int>(it.second);
                break;
            case attr_key::hint_expected_A_size:
                dnnl_attrs.hint_expected_A_size = it.second;
                break;
            case attr_key::hint_expected_B_size:
                dnnl_attrs.hint_expected_B_size = it.second;
                break;
            case attr_key::hint_expected_C_size:
                dnnl_attrs.hint_expected_C_size = it.second;
                break;
            case attr_key::hint_innermost_loop:
                dnnl_attrs.hint_innermost_loop
                        = static_cast<brgemm_kernel_innermost_loop_t>(
                                it.second);
                break;
            case attr_key::hint_loop_order:
                dnnl_attrs.hint_loop_order
                        = static_cast<brgemm_kernel_loop_order_t>(it.second);
                break;
            case attr_key::hint_prefetching:
                dnnl_attrs.hint_prefetching
                        = static_cast<brgemm_kernel_prefetching_t>(it.second);
                break;
            case attr_key::wary_tail_read:
                dnnl_attrs.wary_tail_read = static_cast<bool>(it.second);
                break;
            case attr_key::generate_skip_accumulation:
                dnnl_attrs.generate_skip_accumulation
                        = static_cast<bool>(it.second);
                break;
            case attr_key::bd_mask_level:
                dnnl_attrs.bd_mask_level = static_cast<int>(it.second);
                break;
            case attr_key::use_uker:
                dnnl_attrs.use_uker = static_cast<bool>(it.second);
                break;
            case attr_key::use_interleave_stores:
                dnnl_attrs.use_interleave_stores = static_cast<bool>(it.second);
                break;
            case attr_key::nkeys: break;
        }
    }
    return dnnl_attrs;
}

static dnnl_memory_desc_t get_dst_default_md(int M, int N, int dtype) {
    dnnl_memory_desc_t md;
    memset(&md, 0, sizeof(md));
    md.ndims = 2;
    md.dims[0] = static_cast<dnnl_dim_t>(M);
    md.dims[1] = static_cast<dnnl_dim_t>(N);
    md.padded_dims[0] = md.dims[0];
    md.padded_dims[1] = md.dims[1];
    md.format_kind = dnnl_format_kind_t::dnnl_blocked;
    md.data_type = convert_dnnl_dtype(dtype);
    md.format_desc.blocking.strides[0] = md.dims[1];
    md.format_desc.blocking.strides[1] = 1;
    return md;
}

static dnnl::impl::post_ops_t get_dnnl_postops_setting(
        const postops_setting_t &ops, dnnl_primitive_attr &pattr,
        dnnl_data_type_t &bias_dt, dnnl_data_type_t &out_dt) {
    dnnl::impl::post_ops_t dnnl_postops;
    dnnl_status_t status = dnnl_status_t::dnnl_success;
    for (int i = 0; i < ops.num_; i++) {
        auto &op = ops.ops_[i];
        auto &alg = op.empty_op_.alg_;
        if (alg == alg_kind_t::bias_add) {
            bias_dt = convert_dnnl_dtype(static_cast<int>(op.bias_op_.dtype_));
        } else if (alg == alg_kind_t::out_scales) {
            status = pattr.output_scales_.set(op.scale_op_.scale_);
        } else if (alg == alg_kind_t::a_zp) {
            status = pattr.zero_points_.set(DNNL_ARG_SRC, op.zp_op_.zp_);
        } else if (alg == alg_kind_t::b_zp) {
            status = pattr.zero_points_.set(DNNL_ARG_WEIGHTS, op.zp_op_.zp_);
        } else if (alg == alg_kind_t::c_zp) {
            status = pattr.zero_points_.set(DNNL_ARG_DST, op.zp_op_.zp_);
        } else if (alg == alg_kind_t::out_dtype) {
            out_dt = convert_dnnl_dtype(static_cast<int>(op.out_op_.dtype_));
        } else if (alg >= alg_kind_t::eltwise_begin
                && alg <= alg_kind_t::eltwise_end) {
            status = dnnl_postops.append_eltwise(op.elt_op_.scale_,
                    static_cast<dnnl_alg_kind_t>(alg), op.elt_op_.alpha_,
                    op.elt_op_.beta_);
        } else if (alg >= alg_kind_t::binary_begin
                && alg <= alg_kind_t::binary_end) {
            dnnl_memory_desc_t bin_md = get_dst_default_md(op.bin_op_.shape_[0],
                    op.bin_op_.shape_[1], static_cast<int>(op.bin_op_.dtype_));
            status = dnnl_postops.append_binary(
                    static_cast<dnnl_alg_kind_t>(alg), &bin_md);
        } else {
            throw std::runtime_error("invalid alg kind!");
        }
        assert(status == dnnl_status_t::dnnl_success);
        SC_UNUSED(status);
    }
    return dnnl_postops;
}

struct alignas(64) brg_arg_t {
    float alpha;
    float beta;
    int LDA;
    int LDB;
    int LDC;
    int M;
    int N;
    int K;
    int stride_a;
    int stride_b;
    brgemm_batch_kind_t brg_type;
    int dtypeA;
    int dtypeB;
    int has_bd_mask;
    int64_t brg_attrs[attrs_setting_t::max_attrs_num] = {0};
    int64_t brg_postops[postops_setting_t::max_postops_num
            * postops_setting_t::op_size / sizeof(int64_t)]
            = {0};
    int64_t pad = 0;
    char bd_mask[];

    brg_arg_t(float alpha, float beta, int LDA, int LDB, int LDC, int M, int N,
            int K, int stride_a, int stride_b, brgemm_batch_kind_t brg_type,
            int dtypeA, int dtypeB, const attrs_setting_t *attrs_setting,
            const postops_setting_t *postops_setting, char *bd_mask_ptr)
        : alpha(alpha)
        , beta(beta)
        , LDA(LDA)
        , LDB(LDB)
        , LDC(LDC)
        , M(M)
        , N(N)
        , K(K)
        , stride_a(stride_a)
        , stride_b(stride_b)
        , brg_type(brg_type)
        , dtypeA(dtypeA)
        , dtypeB(dtypeB)
        , has_bd_mask(0) {
        if (attrs_setting != nullptr) {
            for (int i = 0; i < attrs_setting->num_; i++) {
                const std::pair<attr_key, int64_t> &it = attrs_setting->map_[i];
                brg_attrs[it.first] = it.second;
            }
        }
        if (postops_setting != nullptr) {
            assert(postops_setting->num_ <= postops_setting_t::max_postops_num);
            memset(brg_postops, 0,
                    postops_setting_t::max_postops_num
                            * sizeof(postop_setting_t));
            memcpy(brg_postops, postops_setting->ops_,
                    postops_setting->num_ * sizeof(postop_setting_t));
        }
        if (bd_mask_ptr != nullptr) {
            has_bd_mask = 1;
            memcpy(bd_mask, bd_mask_ptr, M * sizeof(char));
        }
    }

    bool operator==(const brg_arg_t &v) const {
        if (memcmp(this, &v, sizeof(brg_arg_t))) { return false; }
        if (has_bd_mask && memcmp(bd_mask, v.bd_mask, M * sizeof(char))) {
            return false;
        }
        return true;
    }

    size_t get_hash() const {
        static_assert(sizeof(brg_arg_t) == 64 * 5,
                "expecting (64 * 5)-byte size for brg_arg");
        vec_u32x4 v = vec_u32x4(0);
        for (int i = 0; i < static_cast<int>(sizeof(brg_arg_t)) / 16; i += 2) {
            vec_u32x4 v0 = vec_u32x4::load(
                    reinterpret_cast<const uint32_t *>(this) + 4 * i);
            vec_u32x4 v1 = vec_u32x4::load(
                    reinterpret_cast<const uint32_t *>(this) + 4 * (i + 1));
            v0 = v0 ^ (_mm_srli_si128(v1.v, 3));
            v0 = v1 ^ (_mm_srli_si128(v0.v, 2));
            v = v ^ v0;
        }
        size_t ret = 0;
        for (int i = 0; i < 2; i++) {
            ret ^= reinterpret_cast<uint64_t *>(v.raw)[i];
        }
        size_t bd_ret = 0;
        if (has_bd_mask) {
            // todo: optimize bd mask hash against byte-by-byte.
            for (int i = 0; i < M; i++) {
                hash_combine(bd_ret, bd_mask[i]);
            }
        }
        ret = ret ^ bd_ret;
        return ret;
    }
};

namespace std {
template <>
struct hash<brg_arg_t> {
    std::size_t operator()(const brg_arg_t &k) const { return k.get_hash(); }
};
} // namespace std

struct brg_arg_ptr_hash_t {
    std::size_t operator()(const brg_arg_t *k) const { return k->get_hash(); }
};

struct brg_arg_ptr_eq_to_t {
    bool operator()(const brg_arg_t *k, const brg_arg_t *k2) const {
        return *k == *k2;
    }
};

static constexpr int PALETTE_SIZE = 64;
struct palette_ptr_t {
    char *ptr_;

    palette_ptr_t(const char *copied) {
        ptr_ = (char *)aligned_alloc(64, PALETTE_SIZE);
        memcpy(ptr_, copied, PALETTE_SIZE);
    }

    palette_ptr_t(palette_ptr_t &&other) noexcept {
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
    }

    ~palette_ptr_t() {
        if (ptr_) { aligned_free(ptr_); }
    }

    struct hasher_t {
        size_t operator()(const palette_ptr_t &p) const {
            size_t ret = 0;
            for (int i = 0; i < int(PALETTE_SIZE / sizeof(ret)); i++) {
                uint64_t val = ((uint64_t *)(p.ptr_))[i];
                hash_combine(ret, val);
            }
            return ret;
        }
    };

    struct cmper_t {
        bool operator()(const palette_ptr_t &p, const palette_ptr_t &p2) const {
            return !memcmp(p.ptr_, p2.ptr_, PALETTE_SIZE);
        }
    };
};

struct brgemm_kernel_info {
    const char *palette_;
    brgemm_kernel_t *brg_kernel_;
    bool is_amx_;
#ifdef SC_KERNEL_PROFILE
    int32_t flops_;
#endif
    ~brgemm_kernel_info() {
        if (brg_kernel_) {
            brgemm_kernel_destroy(brg_kernel_);
            brg_kernel_ = nullptr;
        }
    }
};

struct brg_desc_safe_t {
    ~brg_desc_safe_t() { // NOLINT
        for (auto &kv : brg_desc_vec_) {
            kv.first->~brg_arg_t();
            aligned_free((void *)kv.first);
        }
    }

    brgemm_kernel_info *get(const brg_arg_t *arg) {
        auto found_kernel = brg_desc_vec_local_.find(arg);
        if (found_kernel != brg_desc_vec_local_.end()) {
            return found_kernel->second;
        }
        return nullptr;
    }

    brgemm_kernel_info *getInstance(float alpha, float beta, int LDA, int LDB,
            int LDC, int M, int N, int K, int stride_a, int stride_b,
            brgemm_batch_kind_t brg_type, int dtypeA, int dtypeB,
            const void *attrs_setting, char *bd_mask,
            const void *postops_setting) {
        size_t arg_sz = sizeof(brg_arg_t) + (bd_mask == nullptr ? 0 : M);
        brg_arg_t *arg_ptr = (brg_arg_t *)(alloca(arg_sz));
        new (arg_ptr) brg_arg_t {alpha, beta, LDA, LDB, LDC, M, N, K, stride_a,
                stride_b, brg_type, dtypeA, dtypeB,
                reinterpret_cast<const attrs_setting_t *>(attrs_setting),
                reinterpret_cast<const postops_setting_t *>(postops_setting),
                bd_mask};
        brgemm_kernel_info *found_kernel = get(arg_ptr);
        // check if the brg_arg is in thread local cache (lock free)
        if (found_kernel) { return found_kernel; }
        // if the brg_arg is not found in thread local cache
        std::lock_guard<std::mutex> guard(lock_);
        auto itr = brg_desc_vec_.find(arg_ptr);
        if (itr != brg_desc_vec_.end()) {
            // double check if it is global kernel cache. If so, update the
            // thread local cache and return
            brg_desc_vec_local_.insert(
                    std::make_pair(itr->first, &itr->second));
            return &itr->second;
        }
        arg_ptr = (brg_arg_t *)(aligned_alloc(64, arg_sz));
        new (arg_ptr) brg_arg_t {alpha, beta, LDA, LDB, LDC, M, N, K, stride_a,
                stride_b, brg_type, dtypeA, dtypeB,
                reinterpret_cast<const attrs_setting_t *>(attrs_setting),
                reinterpret_cast<const postops_setting_t *>(postops_setting),
                bd_mask};
        brg_arg_t &arg = *arg_ptr;
        // If we go here, the kernel is not yet created.
        brgemm_t desc;
        brgemm_strides_t stride_info = {arg.stride_a, arg.stride_b};
        auto dnnl_dtypeA = convert_dnnl_dtype(arg.dtypeA);
        auto dnnl_dtypeB = convert_dnnl_dtype(arg.dtypeB);
        size_t dtype_size = get_dtype_sizeof(arg.dtypeA);
        // todo: this type assignment is caused by lack of tail processing
        // in oneDNN (src/cpu/x64/brgemm/brgemm.cpp:305)
        auto choose_isa_type = [&]() {
            if (dnnl_dtypeA != dnnl_f32 && (arg.K < (4 / (int)dtype_size)))
                return avx512_core_vnni;
            int rd_block = dnnl_dtypeA == dnnl_bf16
                    ? 32
                    : (dnnl_dtypeA == dnnl_s8 || dnnl_dtypeA == dnnl_u8) ? 64
                                                                         : -1;
            // when no need for amx:
            if (rd_block == -1) { return isa_any; }
            int rdb = arg.K / rd_block;
            int rdb_tail = arg.K % rd_block;
            int dtype_block = rd_block == 32 ? 2 : 4;
            // if somehow invalid config for amx was generated anyway, make sure
            // it runs on vnni, which has less constraints
            if (rdb > 0 && rdb_tail > 0) { return avx512_core_vnni; }
            if (rdb_tail % dtype_block) { return avx512_core_vnni; }
            return isa_any;
        };
        cpu_isa_t isa_type = choose_isa_type();

        auto status = brgemm_desc_init(&desc, isa_type, arg.brg_type,
                dnnl_dtypeA, dnnl_dtypeB, false, false, brgemm_row_major,
                arg.alpha, arg.beta, arg.LDA, arg.LDB, arg.LDC, arg.M, arg.N,
                arg.K, &stride_info);
        assert(status == dnnl::impl::status::success);

        // create an entry in kernel cache
        auto new_itr = brg_desc_vec_.insert(
                std::make_pair(arg_ptr, brgemm_kernel_info()));
        // check that the insertion happens
        assert(new_itr.second);
        found_kernel = &new_itr.first->second;
        // insert the kernel to thread local cache
        brg_desc_vec_local_.insert(
                std::make_pair(new_itr.first->first, found_kernel));

        // set brgemm attrs
        if (attrs_setting != nullptr || bd_mask != nullptr) {
            brgemm_attr_t dnnl_brg_attrs = get_dnnl_brgemm_attrs(
                    *reinterpret_cast<const attrs_setting_t *>(attrs_setting));
            dnnl_brg_attrs.bd_mask = bd_mask;
            brgemm_desc_set_attr(&desc, dnnl_brg_attrs);
        }

        found_kernel->is_amx_ = false;
        char palette_buffer[PALETTE_SIZE];
        status = brgemm_init_tiles(desc, palette_buffer);
        if (status == dnnl::impl::status::success) {
            auto itr_pair = palettes_.insert(palette_ptr_t(palette_buffer));
            found_kernel->palette_ = itr_pair.first->ptr_;
            amx_tile_configure(found_kernel->palette_);
            found_kernel->is_amx_ = true;
        } else {
            found_kernel->palette_ = nullptr;
        }

        // set brgemm post ops.
        if (postops_setting != nullptr) {
            dnnl_primitive_attr dnnl_pattr;
            dnnl_data_type_t dnnl_bias_dtype
                    = dnnl_data_type_t::dnnl_data_type_undef;
            dnnl_data_type_t dnnl_out_dtype
                    = dnnl_data_type_t::dnnl_data_type_undef;
            dnnl::impl::post_ops_t dnnl_postops_setting
                    = get_dnnl_postops_setting(
                            *reinterpret_cast<const postops_setting_t *>(
                                    postops_setting),
                            dnnl_pattr, dnnl_bias_dtype, dnnl_out_dtype);
            assert(dnnl_out_dtype != dnnl_data_type_t::dnnl_data_type_undef);
            status = dnnl_pattr.set_post_ops(dnnl_postops_setting);
            assert(status == dnnl::impl::status::success);
            // currently we output f32 for all input types.
            dnnl_memory_desc_t dnnl_dst_md = get_dst_default_md(
                    arg.M, arg.N, static_cast<int>(sc_dtype::F32));
            dnnl_dst_md.data_type = dnnl_out_dtype;

            status = brgemm_desc_set_postops(
                    &desc, &dnnl_pattr, &dnnl_dst_md, arg.LDC, dnnl_bias_dtype);
            assert(status == dnnl::impl::status::success);
            // use local vars' lifetime
            status = brgemm_kernel_create(&found_kernel->brg_kernel_, desc);
        } else {
            status = brgemm_kernel_create(&found_kernel->brg_kernel_, desc);
        }
        assert(status == dnnl::impl::status::success);
#ifdef SC_KERNEL_PROFILE
        found_kernel->flops_ = 2 * M * K * N;
#endif
        return found_kernel;
    }

    std::mutex lock_;
    // the table of brgemm argument => brgemm kernel map. It is shared by
    // threads of the same process
    std::unordered_map<const brg_arg_t *, brgemm_kernel_info,
            brg_arg_ptr_hash_t, brg_arg_ptr_eq_to_t>
            brg_desc_vec_;
    std::unordered_set<palette_ptr_t, palette_ptr_t::hasher_t,
            palette_ptr_t::cmper_t>
            palettes_;

    using thread_local_cache = std::unordered_map<const brg_arg_t *,
            brgemm_kernel_info *, brg_arg_ptr_hash_t, brg_arg_ptr_eq_to_t>;
    // the thread local cache of brgemm kernel. The cached key-values are
    // pointers to the key-values in the map above
    static thread_local thread_local_cache brg_desc_vec_local_;
};

static brg_desc_safe_t g_brg_desc_s;
thread_local brg_desc_safe_t::thread_local_cache
        brg_desc_safe_t::brg_desc_vec_local_;

namespace sc {
namespace runtime {

amx_buffer_t::~amx_buffer_t() {
    release();
}
void amx_buffer_t::reset(sc::runtime::stream_t *stream) {
    if (!stream) stream = sc::runtime::get_default_stream();
    engine_ = stream->engine_;
    // Based on jit_brgemm_conv_utils.cpp:2121
    const size_t amx_buf_size = 2 * runtime::get_os_page_size();
    ptr_ = stream->engine_->vtable_->persistent_alloc(
            stream->engine_, amx_buf_size);
}
void amx_buffer_t::release() {
    if (ptr_) {
        assert(engine_);
        engine_->vtable_->persistent_dealloc(engine_, ptr_);
        ptr_ = nullptr;
        engine_ = nullptr;
    }
}
} // namespace runtime
} // namespace sc

static void *get_amx_tile_buf(brgemm_kernel_info *brg_desc,
        sc::runtime::stream_t *stream, bool &amx_exclusive) {
    void *tmp_amx_tile_buf = nullptr;
    if (brg_desc->is_amx_) {
        auto &tls = sc::runtime::thread_local_buffer_t::tls_buffer_;
        amx_exclusive = false;
        if (!amx_exclusive
                || tls.amx_buffer_.cur_palette != brg_desc->palette_) {
            amx_tile_configure(brg_desc->palette_);
            tls.amx_buffer_.cur_palette = brg_desc->palette_;
        }
        auto &amx_tile_buf = tls.amx_buffer_;
        if (!amx_tile_buf.ptr_) { amx_tile_buf.reset(stream); }
        tmp_amx_tile_buf = amx_tile_buf.ptr_;
    }
    return tmp_amx_tile_buf;
}

extern "C" {
SC_API void *dnnl_brgemm_func(int M, int N, int K, int LDA, int LDB, int LDC,
        int stride_a, int stride_b, float beta, int dtypeA, int dtypeB,
        const void *brg_attrs, char *bd_mask, const void *postops_setting) {
    float alpha = 1.0;
    return g_brg_desc_s.getInstance(alpha, beta, LDA, LDB, LDC, M, N, K,
            static_cast<int>(stride_a * get_dtype_sizeof(dtypeA)),
            static_cast<int>(stride_b * get_dtype_sizeof(dtypeB)), brgemm_strd,
            dtypeA, dtypeB, brg_attrs, bd_mask, postops_setting);
}

SC_API void dnnl_brgemm_call(brgemm_kernel_info *brg_desc, const void *A,
        const void *B, void *C, int num, sc::runtime::stream_t *stream) {
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    brgemm_kernel_execute(brg_desc->brg_kernel_, num, (void **)A, (void **)B,
            nullptr, (void *)C, tmp_amx_tile_buf);
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
}

SC_API void dnnl_brgemm_call_postops(brgemm_kernel_info *brg_desc,
        const void *A, const void *B, void *C, int num,
        const void *postops_data, void *c_buf, sc::runtime::stream_t *stream) {
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    brgemm_kernel_execute_postops(brg_desc->brg_kernel_, num, (void **)A,
            (void **)B, nullptr, (void *)c_buf, (void *)C,
            *reinterpret_cast<const brgemm_post_ops_data_t *>(postops_data),
            tmp_amx_tile_buf);
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
}

SC_API void *dnnl_brgemm_list_func(int M, int N, int K, int LDA, int LDB,
        int LDC, float beta, int dtypeA, int dtypeB, const void *brg_attrs,
        char *bd_mask, const void *postops_setting) {
    float alpha = 1.0;
    if (M <= 0) { return nullptr; }
    return g_brg_desc_s.getInstance(alpha, beta, LDA, LDB, LDC, M, N, K, 0, 0,
            brgemm_addr, dtypeA, dtypeB, brg_attrs, bd_mask, postops_setting);
}

SC_API void dnnl_brgemm_list_call(brgemm_kernel_info *brg_desc,
        const void **A_list, const void **B_list, void *C, int num,
        int stride_a, int stride_b, int len, int dtypeA, int dtypeB,
        sc::runtime::stream_t *stream) {
    const int batch_num = num * len;
#ifdef _MSC_VER
    brgemm_batch_element_t *batch
            = (brgemm_batch_element_t *)_malloca(batch_num);
#else
#if CLANGVERSION <= 3
    std::unique_ptr<brgemm_batch_element_t[]> batch_v(
            new brgemm_batch_element_t[batch_num]);
    brgemm_batch_element_t *batch = batch_v.get();
#else
    brgemm_batch_element_t batch[batch_num]; // NOLINT
#endif
#endif

    int sizeofA = get_dtype_sizeof(dtypeA);
    int sizeofB = get_dtype_sizeof(dtypeB);
    for (int i = 0; i < len; ++i) {
        for (int j = 0; j < num; ++j) {
            batch[i * num + j].ptr.A
                    = (((char **)A_list)[i] + (j * stride_a * sizeofA));
            batch[i * num + j].ptr.B
                    = (((char **)B_list)[i] + (j * stride_b * sizeofB));
        }
    }
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, batch_num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    brgemm_kernel_execute(brg_desc->brg_kernel_, batch_num, batch, (void *)C,
            tmp_amx_tile_buf);
#ifdef _MSC_VER
    _freea(batch);
#endif
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
}

SC_API void dnnl_brgemm_list_call_postops(brgemm_kernel_info *brg_desc,
        const void **A_list, const void **B_list, void *C, int num,
        int stride_a, int stride_b, int len, int dtypeA, int dtypeB,
        const void *postops_data, void *c_buf, sc::runtime::stream_t *stream) {
    const int batch_num = num * len;
#ifdef _MSC_VER
    brgemm_batch_element_t *batch
            = (brgemm_batch_element_t *)_malloca(batch_num);
#else
#if CLANGVERSION <= 3
    std::unique_ptr<brgemm_batch_element_t[]> batch_v(
            new brgemm_batch_element_t[batch_num]);
    brgemm_batch_element_t *batch = batch_v.get();
#else
    brgemm_batch_element_t batch[batch_num]; // NOLINT
#endif
#endif

    int sizeofA = get_dtype_sizeof(dtypeA);
    int sizeofB = get_dtype_sizeof(dtypeB);
    for (int i = 0; i < len; ++i) {
        for (int j = 0; j < num; ++j) {
            batch[i * num + j].ptr.A
                    = (((char **)A_list)[i] + (j * stride_a * sizeofA));
            batch[i * num + j].ptr.B
                    = (((char **)B_list)[i] + (j * stride_b * sizeofB));
        }
    }
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, batch_num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    brgemm_kernel_execute_postops(brg_desc->brg_kernel_, batch_num, batch,
            (void *)c_buf, (void *)C,
            *reinterpret_cast<const brgemm_post_ops_data_t *>(postops_data),
            tmp_amx_tile_buf);
#ifdef _MSC_VER
    _freea(batch);
#endif
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
}

#define BRGEMM_DTYPE_INIT(dtype) \
    if (LDC == N) { \
        memset(C, (dtype)value, M *N *get_dtype_sizeof(dtypeC)); \
    } else { \
        for (int i = 0; i < M; ++i) { \
            for (int j = 0; j < N; ++j) { \
                ((dtype *)C)[i * LDC + j] = (dtype)value; \
            } \
        } \
    }

SC_API int dnnl_brgemm_init(
        void *C, int M, int N, int LDC, int dtypeC, float value) {
    if (get_dtype_sizeof(dtypeC) == 1) {
        BRGEMM_DTYPE_INIT(uint8_t);
    } else {
        BRGEMM_DTYPE_INIT(int32_t);
    }
    return 0;
}

SC_API int dnnl_brgemm_init_update(const void *A, const void *B, void *C,
        int num, int M, int N, int K, int LDA, int LDB, int LDC, int stride_a,
        int stride_b, int dtypeA, int dtypeB, const void *brg_attrs,
        char *bd_mask, const void *postops_setting, const void *postops_data,
        void *c_buf, sc::runtime::stream_t *stream) {
    float alpha = 1.0, beta = 0.0;
    auto brg_desc = g_brg_desc_s.getInstance(alpha, beta, LDA, LDB, LDC, M, N,
            K, static_cast<int>(stride_a * get_dtype_sizeof(dtypeA)),
            static_cast<int>(stride_b * get_dtype_sizeof(dtypeB)), brgemm_strd,
            dtypeA, dtypeB, brg_attrs, bd_mask, postops_setting);
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    if (postops_setting == nullptr) {
        brgemm_kernel_execute(brg_desc->brg_kernel_, num, (void **)A,
                (void **)B, nullptr, (void *)C, tmp_amx_tile_buf);
    } else {
        brgemm_kernel_execute_postops(brg_desc->brg_kernel_, num, (void **)A,
                (void **)B, nullptr, (void *)c_buf, (void *)C,
                *reinterpret_cast<const brgemm_post_ops_data_t *>(postops_data),
                tmp_amx_tile_buf);
    }
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
    return 0;
}

SC_API int dnnl_brgemm_update(const void *A, const void *B, void *C, int num,
        int M, int N, int K, int LDA, int LDB, int LDC, int stride_a,
        int stride_b, int dtypeA, int dtypeB, const void *brg_attrs,
        char *bd_mask, const void *postops_setting, const void *postops_data,
        void *c_buf, sc::runtime::stream_t *stream) {
    float alpha = 1.0, beta = 1.0;
    auto brg_desc = g_brg_desc_s.getInstance(alpha, beta, LDA, LDB, LDC, M, N,
            K, static_cast<int>(stride_a * get_dtype_sizeof(dtypeA)),
            static_cast<int>(stride_b * get_dtype_sizeof(dtypeB)), brgemm_strd,
            dtypeA, dtypeB, brg_attrs, bd_mask, postops_setting);
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    if (postops_setting == nullptr) {
        brgemm_kernel_execute(brg_desc->brg_kernel_, num, (void **)A,
                (void **)B, nullptr, (void *)C, tmp_amx_tile_buf);
    } else {
        brgemm_kernel_execute_postops(brg_desc->brg_kernel_, num, (void **)A,
                (void **)B, nullptr, (void *)c_buf, (void *)C,
                *reinterpret_cast<const brgemm_post_ops_data_t *>(postops_data),
                tmp_amx_tile_buf);
    }
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
    return 0;
}

SC_API int dnnl_brgemm_list_update(const void **A_list, const void **B_list,
        void *C, int num, int M, int N, int K, int LDA, int LDB, int LDC,
        int stride_a, int stride_b, int len, int dtypeA, int dtypeB,
        const void *brg_attrs, char *bd_mask, const void *postops_setting,
        const void *postops_data, void *c_buf, sc::runtime::stream_t *stream) {
    float alpha = 1.0, beta = 1.0;
    const int batch_num = num * len;
#ifdef _MSC_VER
    brgemm_batch_element_t *batch
            = (brgemm_batch_element_t *)_malloca(batch_num);
#else
#if CLANGVERSION <= 3
    std::unique_ptr<brgemm_batch_element_t[]> batch_v(
            new brgemm_batch_element_t[batch_num]);
    brgemm_batch_element_t *batch = batch_v.get();
#else
    brgemm_batch_element_t batch[batch_num]; // NOLINT
#endif
#endif
    int sizeofA = get_dtype_sizeof(dtypeA);
    int sizeofB = get_dtype_sizeof(dtypeB);
    for (int i = 0; i < len; ++i) {
        for (int j = 0; j < num; ++j) {
            batch[i * num + j].ptr.A
                    = (((char **)A_list)[i] + (j * stride_a * sizeofA));
            batch[i * num + j].ptr.B
                    = (((char **)B_list)[i] + (j * stride_b * sizeofB));
        }
    }
    auto brg_desc = g_brg_desc_s.getInstance(alpha, beta, LDA, LDB, LDC, M, N,
            K, 0, 0, brgemm_addr, dtypeA, dtypeB, brg_attrs, bd_mask,
            postops_setting);
    bool amx_exclusive = false;
    sc_make_timer(brg_desc, batch_num);
    void *tmp_amx_tile_buf = get_amx_tile_buf(brg_desc, stream, amx_exclusive);
    if (postops_setting == nullptr) {
        brgemm_kernel_execute(brg_desc->brg_kernel_, batch_num, batch,
                (void *)C, tmp_amx_tile_buf);
    } else {
        brgemm_kernel_execute_postops(brg_desc->brg_kernel_, batch_num, batch,
                (void *)c_buf, (void *)C,
                *reinterpret_cast<const brgemm_post_ops_data_t *>(postops_data),
                tmp_amx_tile_buf);
    }
#ifdef _MSC_VER
    _freea(batch);
#endif
    if (!amx_exclusive && brg_desc->is_amx_) { amx_tile_release(); }
    return 0;
}
SC_API void dnnl_brgemm_postops_data_init(void *dnnl_data, void *bias,
        void *scales, void *binary_post_ops_rhs, uint64_t oc_logical_off,
        uint64_t dst_row_logical_off, void *data_C_ptr_,
        uint64_t first_mb_matrix_addr_off, void *a_zp_compensations,
        void *b_zp_compensations, void *c_zp_values, bool skip_accumulation) {
    brgemm_post_ops_data_t *postop_data
            = reinterpret_cast<brgemm_post_ops_data_t *>(dnnl_data);
    new (postop_data)
            brgemm_post_ops_data_t {bias, reinterpret_cast<float *>(scales),
                    binary_post_ops_rhs, oc_logical_off, dst_row_logical_off,
                    reinterpret_cast<char *>(data_C_ptr_),
                    first_mb_matrix_addr_off, a_zp_compensations,
                    b_zp_compensations, c_zp_values, skip_accumulation};
}
}
