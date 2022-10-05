/*
 * Copyright © 2009 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Red Hat not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Red Hat makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#include "pixman-private.h"

#if defined(TT_MEMUTIL) && defined(_MSC_VER)
#include <omp.h>
#endif

#ifdef _MSC_VER
#include <windows.h>
#endif

pixman_implementation_t *
_pixman_implementation_create (pixman_implementation_t *fallback,
			       const pixman_fast_path_t *fast_paths)
{
    pixman_implementation_t *imp;

    assert (fast_paths);

    if ((imp = malloc (sizeof (pixman_implementation_t))))
    {
	pixman_implementation_t *d;

	memset (imp, 0, sizeof *imp);

	imp->fallback = fallback;
	imp->fast_paths = fast_paths;
	
	/* Make sure the whole fallback chain has the right toplevel */
	for (d = imp; d != NULL; d = d->fallback)
	    d->toplevel = imp;
    }

    return imp;
}

#define N_CACHED_FAST_PATHS 8

typedef struct
{
    struct
    {
	pixman_implementation_t *	imp;
	pixman_fast_path_t		fast_path;
    } cache [N_CACHED_FAST_PATHS];
} cache_t;

PIXMAN_DEFINE_THREAD_LOCAL (cache_t, fast_path_cache);

static void
dummy_composite_rect (pixman_implementation_t *imp,
		      pixman_composite_info_t *info)
{
}

void
_pixman_implementation_lookup_composite (pixman_implementation_t  *toplevel,
					 pixman_op_t               op,
					 pixman_format_code_t      src_format,
					 uint32_t                  src_flags,
					 pixman_format_code_t      mask_format,
					 uint32_t                  mask_flags,
					 pixman_format_code_t      dest_format,
					 uint32_t                  dest_flags,
					 pixman_implementation_t **out_imp,
					 pixman_composite_func_t  *out_func)
{
    pixman_implementation_t *imp;
    cache_t *cache;
    int i;

    /* Check cache for fast paths */
    cache = PIXMAN_GET_THREAD_LOCAL (fast_path_cache);

    for (i = 0; i < N_CACHED_FAST_PATHS; ++i)
    {
	const pixman_fast_path_t *info = &(cache->cache[i].fast_path);

	/* Note that we check for equality here, not whether
	 * the cached fast path matches. This is to prevent
	 * us from selecting an overly general fast path
	 * when a more specific one would work.
	 */
	if (info->op == op			&&
	    info->src_format == src_format	&&
	    info->mask_format == mask_format	&&
	    info->dest_format == dest_format	&&
	    info->src_flags == src_flags	&&
	    info->mask_flags == mask_flags	&&
	    info->dest_flags == dest_flags	&&
	    info->func)
	{
	    *out_imp = cache->cache[i].imp;
	    *out_func = cache->cache[i].fast_path.func;

	    goto update_cache;
	}
    }

    for (imp = toplevel; imp != NULL; imp = imp->fallback)
    {
	const pixman_fast_path_t *info = imp->fast_paths;

	while (info->op != PIXMAN_OP_NONE)
	{
	    if ((info->op == op || info->op == PIXMAN_OP_any)		&&
		/* Formats */
		((info->src_format == src_format) ||
		 (info->src_format == PIXMAN_any))			&&
		((info->mask_format == mask_format) ||
		 (info->mask_format == PIXMAN_any))			&&
		((info->dest_format == dest_format) ||
		 (info->dest_format == PIXMAN_any))			&&
		/* Flags */
		(info->src_flags & src_flags) == info->src_flags	&&
		(info->mask_flags & mask_flags) == info->mask_flags	&&
		(info->dest_flags & dest_flags) == info->dest_flags)
	    {
		*out_imp = imp;
		*out_func = info->func;

		/* Set i to the last spot in the cache so that the
		 * move-to-front code below will work
		 */
		i = N_CACHED_FAST_PATHS - 1;

		goto update_cache;
	    }

	    ++info;
	}
    }

    /* We should never reach this point */
    _pixman_log_error (FUNC, "No known composite function\n");
    *out_imp = NULL;
    *out_func = dummy_composite_rect;

update_cache:
    if (i)
    {
	while (i--)
	    cache->cache[i + 1] = cache->cache[i];

	cache->cache[0].imp = *out_imp;
	cache->cache[0].fast_path.op = op;
	cache->cache[0].fast_path.src_format = src_format;
	cache->cache[0].fast_path.src_flags = src_flags;
	cache->cache[0].fast_path.mask_format = mask_format;
	cache->cache[0].fast_path.mask_flags = mask_flags;
	cache->cache[0].fast_path.dest_format = dest_format;
	cache->cache[0].fast_path.dest_flags = dest_flags;
	cache->cache[0].fast_path.func = *out_func;
    }
}

static void
dummy_combine (pixman_implementation_t *imp,
	       pixman_op_t              op,
	       uint32_t *               pd,
	       const uint32_t *         ps,
	       const uint32_t *         pm,
	       int                      w)
{
}

pixman_combine_32_func_t
_pixman_implementation_lookup_combiner (pixman_implementation_t *imp,
					pixman_op_t		 op,
					pixman_bool_t		 component_alpha,
					pixman_bool_t		 narrow,
					pixman_bool_t		 rgb16)
{
    while (imp)
    {
	pixman_combine_32_func_t f = NULL;

	switch ((narrow << 1) | component_alpha)
	{
	case 0: /* not narrow, not component alpha */
	    f = (pixman_combine_32_func_t)imp->combine_float[op];
	    break;
	    
	case 1: /* not narrow, component_alpha */
	    f = (pixman_combine_32_func_t)imp->combine_float_ca[op];
	    break;

	case 2: /* narrow, not component alpha */
	    f = imp->combine_32[op];
	    break;

	case 3: /* narrow, component_alpha */
	    f = imp->combine_32_ca[op];
	    break;
	}
	if (rgb16)
	    f = (pixman_combine_32_func_t *)imp->combine_16[op];

	if (f)
	    return f;

	imp = imp->fallback;
    }

    /* We should never reach this point */
    _pixman_log_error (FUNC, "No known combine function\n");
    return dummy_combine;
}

pixman_bool_t
_pixman_implementation_blt (pixman_implementation_t * imp,
                            uint32_t *                src_bits,
                            uint32_t *                dst_bits,
                            int                       src_stride,
                            int                       dst_stride,
                            int                       src_bpp,
                            int                       dst_bpp,
                            int                       src_x,
                            int                       src_y,
                            int                       dest_x,
                            int                       dest_y,
                            int                       width,
                            int                       height)
{
    while (imp)
    {
	if (imp->blt &&
	    (*imp->blt) (imp, src_bits, dst_bits, src_stride, dst_stride,
			 src_bpp, dst_bpp, src_x, src_y, dest_x, dest_y,
			 width, height))
	{
	    return TRUE;
	}

	imp = imp->fallback;
    }

    return FALSE;
}

pixman_bool_t
_pixman_implementation_fill (pixman_implementation_t *imp,
                             uint32_t *               bits,
                             int                      stride,
                             int                      bpp,
                             int                      x,
                             int                      y,
                             int                      width,
                             int                      height,
                             uint32_t                 filler)
{
    while (imp)
    {
	if (imp->fill &&
	    ((*imp->fill) (imp, bits, stride, bpp, x, y, width, height, filler)))
	{
	    return TRUE;
	}

	imp = imp->fallback;
    }

    return FALSE;
}

pixman_bool_t
_pixman_implementation_src_iter_init (pixman_implementation_t	*imp,
				      pixman_iter_t             *iter,
				      pixman_image_t		*image,
				      int			 x,
				      int			 y,
				      int			 width,
				      int			 height,
				      uint8_t			*buffer,
				      iter_flags_t		 iter_flags,
				      uint32_t                   image_flags)
{
    iter->image = image;
    iter->buffer = (uint32_t *)buffer;
    iter->x = x;
    iter->y = y;
    iter->width = width;
    iter->height = height;
    iter->iter_flags = iter_flags;
    iter->image_flags = image_flags;

    while (imp)
    {
	if (imp->src_iter_init && (*imp->src_iter_init) (imp, iter))
	    return TRUE;

	imp = imp->fallback;
    }

    return FALSE;
}

pixman_bool_t
_pixman_implementation_dest_iter_init (pixman_implementation_t	*imp,
				       pixman_iter_t            *iter,
				       pixman_image_t		*image,
				       int			 x,
				       int			 y,
				       int			 width,
				       int			 height,
				       uint8_t			*buffer,
				       iter_flags_t		 iter_flags,
				       uint32_t                  image_flags)
{
    iter->image = image;
    iter->buffer = (uint32_t *)buffer;
    iter->x = x;
    iter->y = y;
    iter->width = width;
    iter->height = height;
    iter->iter_flags = iter_flags;
    iter->image_flags = image_flags;

    while (imp)
    {
	if (imp->dest_iter_init && (*imp->dest_iter_init) (imp, iter))
	    return TRUE;

	imp = imp->fallback;
    }

    return FALSE;
}

pixman_bool_t
_pixman_disabled (const char *name)
{
    const char *env;

    if ((env = getenv ("PIXMAN_DISABLE")))
    {
	do
	{
	    const char *end;
	    int len;

	    if ((end = strchr (env, ' ')))
		len = end - env;
	    else
		len = strlen (env);

	    if (strlen (name) == len && strncmp (name, env, len) == 0)
	    {
		printf ("pixman: Disabled %s implementation\n", name);
		return TRUE;
	    }

	    env += len;
	}
	while (*env++);
    }

    return FALSE;
}

#ifdef _MSC_VER

#ifdef TT_MEMUTIL
uint32_t dwNonTemporalDataSizeMin = NON_TEMPORAL_STORES_NOT_SUPPORTED;
uint32_t dwNonTemporalMemcpySizeMin = NON_TEMPORAL_STORES_NOT_SUPPORTED;
#endif
typedef BOOL (WINAPI *LPFN_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

int Initialize_TT()
{
#ifdef TT_MEMUTIL
    int omp_thread_counts = 0;
    DWORD pam, sam;

    long env_omp_num_threads = 0;
    wchar_t *lpwz_env = _wgetenv(L"OMP_NUM_THREADS");
    if (lpwz_env)
    {
      env_omp_num_threads = _wtol(lpwz_env);
    }

    omp_set_dynamic(0);
    omp_set_num_threads(1);

    if (GetProcessAffinityMask(GetCurrentProcess(), &pam, &sam))
    {
        LPFN_GLPI glpi =
            (LPFN_GLPI)GetProcAddress(GetModuleHandle("kernel32.dll"),
            "GetLogicalProcessorInformation");
        DWORD returnLength = 0;
        int *pThreadBindIndex = NULL;

        if (NULL != glpi &&
            !glpi(NULL, &returnLength) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer =
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(returnLength);

            if (glpi(buffer, &returnLength))
            {
                DWORD byteOffset;
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr;
                int i;
                size_t threadBindIndexSize;

                byteOffset = 0;
                ptr = buffer;
                while (byteOffset < returnLength)
                {
                    if (RelationProcessorCore == ptr->Relationship)
                    {
                        omp_thread_counts++;
                    }
                    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                    ptr++;
                }

                threadBindIndexSize = sizeof(int) * omp_thread_counts;
                pThreadBindIndex = (int *)malloc(threadBindIndexSize);
                memset(pThreadBindIndex, 0xFF, threadBindIndexSize);

                i = 0;
                byteOffset = 0;
                ptr = buffer;
                while (byteOffset < returnLength)
                {
                    if (RelationProcessorCore == ptr->Relationship)
                    {
                        if (i < omp_thread_counts)
                        {
                            int b;

                            for (b = 0; b <= 31; b++)
                            {
                                if ((pam & ptr->ProcessorMask) & (1 << b))
                                {
                                    pThreadBindIndex[i++] = b;
                                    break;
                                }
                            }
                        }
                        else
                        {
                            break;
                        }
                    }
                    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                    ptr++;
                }
            }
            free(buffer);
        }

        if (NULL == pThreadBindIndex)
        {
            int b;
            int i;
            size_t threadBindIndexSize;

            for (b = 0; b <= 31; b++)
            {
                if (pam & (1 << b)) omp_thread_counts++;
            }

            threadBindIndexSize = sizeof(int) * omp_thread_counts;
            pThreadBindIndex = (int *)malloc(threadBindIndexSize);
            memset(pThreadBindIndex, 0xFF, threadBindIndexSize);

            for (i = 0; i < omp_thread_counts; i++)
            {
                pThreadBindIndex[i] = i;
            }
        }

        if (NULL != pThreadBindIndex)
        {
            if (omp_thread_counts >= 1)
            {
                OSVERSIONINFO osvi = { sizeof(OSVERSIONINFO) };
                BOOL bIsWindows7orLater = FALSE;

                omp_set_dynamic(0);
                if (0 != env_omp_num_threads)
                {
                    omp_thread_counts = env_omp_num_threads;
                }
                omp_set_num_threads(omp_thread_counts);
                omp_thread_counts = omp_get_max_threads();

                GetVersionEx(&osvi);
                bIsWindows7orLater =
                    (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId) &&
                    ((6 == osvi.dwMajorVersion && osvi.dwMinorVersion >= 1) || (osvi.dwMajorVersion >= 7));
                if (!bIsWindows7orLater)
                {
#pragma omp parallel
                    {
                        SetThreadIdealProcessor(GetCurrentThread(),
                            pThreadBindIndex[omp_get_thread_num()]);
                    }
                }
            }
            free(pThreadBindIndex);
        }
    }
#endif /* TT_MEMUTIL */

#ifdef TT_MEMUTIL
    dwNonTemporalMemcpySizeMin = dwNonTemporalDataSizeMin = GetNonTemporalDataSizeMin_tt();
    if (dwNonTemporalMemcpySizeMin != NON_TEMPORAL_STORES_NOT_SUPPORTED)
    {
        dwNonTemporalMemcpySizeMin = dwNonTemporalDataSizeMin / 2;
    }
#endif

    return 0;
}

#endif /* _MSC_VER */

pixman_implementation_t *
_pixman_choose_implementation (void)
{
    pixman_implementation_t *imp;

#ifdef _MSC_VER
    {
        static pixman_bool_t initialized = FALSE;

        if (!initialized)
        {
            Initialize_TT();
            initialized = TRUE;
        }
    }
#endif

    imp = _pixman_implementation_create_general();

    if (!_pixman_disabled ("fast"))
	imp = _pixman_implementation_create_fast_path (imp);

    imp = _pixman_x86_get_implementations (imp);
    imp = _pixman_arm_get_implementations (imp);
    imp = _pixman_ppc_get_implementations (imp);
    imp = _pixman_mips_get_implementations (imp);

    imp = _pixman_implementation_create_noop (imp);

    return imp;
}
