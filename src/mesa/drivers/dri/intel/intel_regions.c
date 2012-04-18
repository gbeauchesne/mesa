/**************************************************************************
 * 
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/* Provide additional functionality on top of bufmgr buffers:
 *   - 2d semantics and blit operations
 *   - refcounting of buffers for multiple images in a buffer.
 *   - refcounting of buffer mappings.
 *   - some logic for moving the buffers to the best memory pools for
 *     given operations.
 *
 * Most of this is to make it easier to implement the fixed-layout
 * mipmap tree required by intel hardware in the face of GL's
 * programming interface where each image can be specifed in random
 * order and it isn't clear what layout the tree should have until the
 * last moment.
 */

#include <sys/ioctl.h>
#include <errno.h>

#include "main/hash.h"
#include "intel_context.h"
#include "intel_regions.h"
#include "intel_blit.h"
#include "intel_buffer_objects.h"
#include "intel_bufmgr.h"
#include "intel_batchbuffer.h"

#define FILE_DEBUG_FLAG DEBUG_REGION

/* This should be set to the maximum backtrace size desired.
 * Set it to 0 to disable backtrace debugging.
 */
#define DEBUG_BACKTRACE_SIZE 0

#if DEBUG_BACKTRACE_SIZE == 0
/* Use the standard debug output */
#define _DBG(...) DBG(__VA_ARGS__)
#else
/* Use backtracing debug output */
#define _DBG(...) {debug_backtrace(); DBG(__VA_ARGS__);}

/* Backtracing debug support */
#include <execinfo.h>

static void
debug_backtrace(void)
{
   void *trace[DEBUG_BACKTRACE_SIZE];
   char **strings = NULL;
   int traceSize;
   register int i;

   traceSize = backtrace(trace, DEBUG_BACKTRACE_SIZE);
   strings = backtrace_symbols(trace, traceSize);
   if (strings == NULL) {
      DBG("no backtrace:");
      return;
   }

   /* Spit out all the strings with a colon separator.  Ignore
    * the first, since we don't really care about the call
    * to debug_backtrace() itself.  Skip until the final "/" in
    * the trace to avoid really long lines.
    */
   for (i = 1; i < traceSize; i++) {
      char *p = strings[i], *slash = strings[i];
      while (*p) {
         if (*p++ == '/') {
            slash = p;
         }
      }

      DBG("%s:", slash);
   }

   /* Free up the memory, and we're done */
   free(strings);
}

#endif

/* Region hash utilities */
#define MAX_PLANES 3

struct hash_entry {
    struct intel_region *regions[MAX_PLANES];
    unsigned int num_regions;
};

static inline struct hash_entry *
hash_entry_new(void)
{
    return calloc(1, sizeof(struct hash_entry));
}

static inline void
hash_entry_destroy(struct hash_entry *e)
{
    free(e);
}

static void
intel_region_hash_destroy_callback(GLuint key, void *data, void *userData)
{
    hash_entry_destroy(data);
}

struct _mesa_HashTable *
intel_region_hash_new(void)
{
    return _mesa_NewHashTable();
}

void
intel_region_hash_destroy(struct _mesa_HashTable **h_ptr)
{
    struct _mesa_HashTable * const h = *h_ptr;

   /* Some regions may still have references to them at this point, so
    * flush the hash table to prevent _mesa_DeleteHashTable() from
    * complaining about the hash not being empty; */
   _mesa_HashDeleteAll(h, intel_region_hash_destroy_callback, NULL);
   _mesa_DeleteHashTable(h);
   *h_ptr = NULL;
}

static inline struct intel_region *
intel_region_hash_lookup(struct _mesa_HashTable *h, uint32_t name, uint32_t id)
{
    struct hash_entry * const e = _mesa_HashLookup(h, name);

    if (e && id < ARRAY_SIZE(e->regions))
        return e->regions[id];
    return NULL;
}

static void
intel_region_hash_insert(struct _mesa_HashTable *h, struct intel_region *region)
{
    struct hash_entry *e = _mesa_HashLookup(h, region->name);

    if (!e) {
        e = hash_entry_new();
        if (!e)
            return;
        _mesa_HashInsert(h, region->name, e);
    }

    if (region->plane_id < ARRAY_SIZE(e->regions)) {
        if (!e->regions[region->plane_id])
            e->num_regions++;
        e->regions[region->plane_id] = region;
    }
}

static void
intel_region_hash_remove(struct _mesa_HashTable *h, struct intel_region *region)
{
    struct hash_entry * const e = _mesa_HashLookup(h, region->name);

    if (e && region->plane_id < ARRAY_SIZE(e->regions)) {
        e->regions[region->plane_id] = NULL;
        e->num_regions--;
    }

    if (e->num_regions == 0) {
        _mesa_HashRemove(h, region->name);
        hash_entry_destroy(e);
    }
}

/* XXX: Thread safety?
 */
void *
intel_region_map(struct intel_context *intel, struct intel_region *region,
                 GLbitfield mode)
{
   /* We have the region->map_refcount controlling mapping of the BO because
    * in software fallbacks we may end up mapping the same buffer multiple
    * times on Mesa's behalf, so we refcount our mappings to make sure that
    * the pointer stays valid until the end of the unmap chain.  However, we
    * must not emit any batchbuffers between the start of mapping and the end
    * of unmapping, or further use of the map will be incoherent with the GPU
    * rendering done by that batchbuffer. Hence we assert in
    * intel_batchbuffer_flush() that that doesn't happen, which means that the
    * flush is only needed on first map of the buffer.
    */

   _DBG("%s %p\n", __FUNCTION__, region);
   if (!region->map_refcount) {
      intel_flush(&intel->ctx);

      if (region->tiling != I915_TILING_NONE)
	 drm_intel_gem_bo_map_gtt(region->bo);
      else
	 drm_intel_bo_map(region->bo, true);

      region->map = region->bo->virtual;
   }
   if (region->map) {
      intel->num_mapped_regions++;
      region->map_refcount++;
   }

   return region->map;
}

void
intel_region_unmap(struct intel_context *intel, struct intel_region *region)
{
   _DBG("%s %p\n", __FUNCTION__, region);
   if (!--region->map_refcount) {
      if (region->tiling != I915_TILING_NONE)
	 drm_intel_gem_bo_unmap_gtt(region->bo);
      else
	 drm_intel_bo_unmap(region->bo);

      region->map = NULL;
      --intel->num_mapped_regions;
      assert(intel->num_mapped_regions >= 0);
   }
}

static struct intel_region *
intel_region_alloc_internal(struct intel_screen *screen,
			    const struct intel_region_attributes *attrs,
			    uint32_t tiling, drm_intel_bo *buffer)
{
   struct intel_region *region;

   region = calloc(sizeof(*region), 1);
   if (region == NULL)
      return region;

   region->plane_id = 0;
   region->cpp = attrs->cpp;
   region->width = attrs->width;
   region->height = attrs->height;
   region->pitch = attrs->pitch;
   region->refcount = 1;
   region->bo = buffer;
   region->tiling = tiling;
   region->screen = screen;

   _DBG("%s <-- %p\n", __FUNCTION__, region);
   return region;
}

struct intel_region *
intel_region_alloc(struct intel_screen *screen,
		   uint32_t tiling,
                   GLuint cpp, GLuint width, GLuint height,
		   bool expect_accelerated_upload)
{
   drm_intel_bo *buffer;
   unsigned long flags = 0;
   unsigned long aligned_pitch;
   struct intel_region *region;
   struct intel_region_attributes attrs;

   if (expect_accelerated_upload)
      flags |= BO_ALLOC_FOR_RENDER;

   buffer = drm_intel_bo_alloc_tiled(screen->bufmgr, "region",
				     width, height, cpp,
				     &tiling, &aligned_pitch, flags);
   if (buffer == NULL)
      return NULL;

   attrs.cpp       = cpp;
   attrs.width     = width;
   attrs.height    = height;
   attrs.pitch     = aligned_pitch / cpp;
   region = intel_region_alloc_internal(screen, &attrs, tiling, buffer);
   if (region == NULL) {
      drm_intel_bo_unreference(buffer);
      return NULL;
   }

   return region;
}

bool
intel_region_flink(struct intel_region *region, uint32_t *name)
{
   if (region->name == 0) {
      if (drm_intel_bo_flink(region->bo, &region->name))
	 return false;
      
      intel_region_hash_insert(region->screen->named_regions, region);
   }

   *name = region->name;

   return true;
}

static inline bool
intel_region_validate_attributes(const struct intel_region *region,
                                 const struct intel_region_attributes *attrs)
{
    return (region->cpp    == attrs->cpp    &&
            region->width  == attrs->width  &&
            region->height == attrs->height &&
            region->pitch  == attrs->pitch);
}

struct intel_region *
intel_region_alloc_for_handle2(struct intel_screen *screen,
			       unsigned int handle, const char *name,
			       const struct intel_region_attributes *attrs)
{
   struct intel_region *region, *dummy;
   drm_intel_bo *buffer;
   int ret;
   uint32_t bit_6_swizzle, tiling;

   region = intel_region_hash_lookup(screen->named_regions, handle, 0);
   if (region != NULL) {
      dummy = NULL;
      if (!intel_region_validate_attributes(region, attrs)) {
	 fprintf(stderr,
		 "Region for name %d already exists but is not compatible\n",
		 handle);
	 return NULL;
      }
      intel_region_reference(&dummy, region);
      return dummy;
   }

   buffer = intel_bo_gem_create_from_name(screen->bufmgr, name, handle);
   if (buffer == NULL)
      return NULL;
   ret = drm_intel_bo_get_tiling(buffer, &tiling, &bit_6_swizzle);
   if (ret != 0) {
      fprintf(stderr, "Couldn't get tiling of buffer %d (%s): %s\n",
	      handle, name, strerror(-ret));
      drm_intel_bo_unreference(buffer);
      return NULL;
   }

   region = intel_region_alloc_internal(screen, attrs, tiling, buffer);
   if (region == NULL) {
      drm_intel_bo_unreference(buffer);
      return NULL;
   }

   region->name = handle;
   intel_region_hash_insert(screen->named_regions, region);

   return region;
}

void
intel_region_reference(struct intel_region **dst, struct intel_region *src)
{
   _DBG("%s: %p(%d) -> %p(%d)\n", __FUNCTION__,
	*dst, *dst ? (*dst)->refcount : 0, src, src ? src->refcount : 0);

   if (src != *dst) {
      if (*dst)
	 intel_region_release(dst);

      if (src)
         src->refcount++;
      *dst = src;
   }
}

void
intel_region_release(struct intel_region **region_handle)
{
   struct intel_region *region = *region_handle;

   if (region == NULL) {
      _DBG("%s NULL\n", __FUNCTION__);
      return;
   }

   _DBG("%s %p %d\n", __FUNCTION__, region, region->refcount - 1);

   ASSERT(region->refcount > 0);
   region->refcount--;

   if (region->refcount == 0) {
      assert(region->map_refcount == 0);

      drm_intel_bo_unreference(region->bo);

      if (region->name > 0)
	 intel_region_hash_remove(region->screen->named_regions, region);

      free(region);
   }
   *region_handle = NULL;
}

/*
 * XXX Move this into core Mesa?
 */
void
_mesa_copy_rect(GLubyte * dst,
                GLuint cpp,
                GLuint dst_pitch,
                GLuint dst_x,
                GLuint dst_y,
                GLuint width,
                GLuint height,
                const GLubyte * src,
                GLuint src_pitch, GLuint src_x, GLuint src_y)
{
   GLuint i;

   dst_pitch *= cpp;
   src_pitch *= cpp;
   dst += dst_x * cpp;
   src += src_x * cpp;
   dst += dst_y * dst_pitch;
   src += src_y * src_pitch;
   width *= cpp;

   if (width == dst_pitch && width == src_pitch)
      memcpy(dst, src, height * width);
   else {
      for (i = 0; i < height; i++) {
         memcpy(dst, src, width);
         dst += dst_pitch;
         src += src_pitch;
      }
   }
}

/* Copy rectangular sub-regions. Need better logic about when to
 * push buffers into AGP - will currently do so whenever possible.
 */
bool
intel_region_copy(struct intel_context *intel,
                  struct intel_region *dst,
                  GLuint dst_offset,
                  GLuint dstx, GLuint dsty,
                  struct intel_region *src,
                  GLuint src_offset,
                  GLuint srcx, GLuint srcy, GLuint width, GLuint height,
		  bool flip,
		  GLenum logicop)
{
   uint32_t src_pitch = src->pitch;

   _DBG("%s\n", __FUNCTION__);

   if (intel == NULL)
      return false;

   assert(src->cpp == dst->cpp);

   if (flip)
      src_pitch = -src_pitch;

   return intelEmitCopyBlit(intel,
			    dst->cpp,
			    src_pitch, src->bo, src_offset, src->tiling,
			    dst->pitch, dst->bo, dst_offset, dst->tiling,
			    srcx, srcy, dstx, dsty, width, height,
			    logicop);
}

/**
 * This function computes masks that may be used to select the bits of the X
 * and Y coordinates that indicate the offset within a tile.  If the region is
 * untiled, the masks are set to 0.
 */
void
intel_region_get_tile_masks(struct intel_region *region,
                            uint32_t *mask_x, uint32_t *mask_y)
{
   int cpp = region->cpp;

   switch (region->tiling) {
   default:
      assert(false);
   case I915_TILING_NONE:
      *mask_x = *mask_y = 0;
      break;
   case I915_TILING_X:
      *mask_x = 512 / cpp - 1;
      *mask_y = 7;
      break;
   case I915_TILING_Y:
      *mask_x = 128 / cpp - 1;
      *mask_y = 31;
      break;
   }
}

/**
 * Compute the offset (in bytes) from the start of the region to the given x
 * and y coordinate.  For tiled regions, caller must ensure that x and y are
 * multiples of the tile size.
 */
uint32_t
intel_region_get_aligned_offset(struct intel_region *region, uint32_t x,
                                uint32_t y)
{
   int cpp = region->cpp;
   uint32_t pitch = region->pitch * cpp;

   switch (region->tiling) {
   default:
      assert(false);
   case I915_TILING_NONE:
      return y * pitch + x * cpp;
   case I915_TILING_X:
      assert((x % (512 / cpp)) == 0);
      assert((y % 8) == 0);
      return y * pitch + x / (512 / cpp) * 4096;
   case I915_TILING_Y:
      assert((x % (128 / cpp)) == 0);
      assert((y % 32) == 0);
      return y * pitch + x / (128 / cpp) * 4096;
   }
}
