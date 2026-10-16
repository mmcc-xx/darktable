# feature checks for src/api/compat.h, so that src/api builds against
# darktable master and releases from 5.6.2 on. Each check looks for what a
# darktable has, never for its version number. darktable's own headers
# are read rather than compiled against: they need its whole include path
# and generated headers, which aren't there yet when this runs.

function(dt_api_header_has var header regex)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${header}" _text)
  if(_text MATCHES "${regex}")
    add_definitions(-D${var})
    message(STATUS "darktable-api: ${var}")
  endif()
endfunction()

# dt_iop_commit_blend_params(module, params, pipe): a third argument since 5.8
dt_api_header_has(HAVE_COMMIT_BLEND_PARAMS_PIPE "develop/imageop.h"
                  "dt_iop_commit_blend_params\\([^;]*dt_dev_pixelpipe_t \\*pipe\\)")
# dt_variables_expand_path (dt_variables_expand with Windows separators)
dt_api_header_has(HAVE_DT_VARIABLES_EXPAND_PATH "common/variables.h"
                  "dt_variables_expand_path\\(")
# Xmp.darktable.neural_restore on AI denoise output
dt_api_header_has(HAVE_DT_EXIF_XMP_WRITE_NEURAL_RESTORE "common/exif.h"
                  "dt_exif_xmp_write_neural_restore\\(")
# dt_pipe_processing (else the pipe's processing flag)
dt_api_header_has(HAVE_DT_PIPE_PROCESSING "develop/pixelpipe_hb.h"
                  "dt_pipe_processing\\(")
# composite's high-precision compositing (the overlay in float; since 5.8),
# which ai_fill needs, and the fork's "native" mode on it (the overlay at its
# own size, exact; else high precision places the patch to within a level)
dt_api_header_has(HAVE_OVERLAY_COMPOSITE_HQ "iop/overlay.c"
                  "DT_OVERLAY_COMPOSITE_HQ")
dt_api_header_has(HAVE_OVERLAY_COMPOSITE_NATIVE "iop/overlay.c"
                  "DT_OVERLAY_COMPOSITE_NATIVE")
# flexi masks (master, 5.8 on): a module's mask is a tree of groups, each
# with a marker holding its operator (dev-doc/masks_data_model.md)
dt_api_header_has(HAVE_MASKS_FLEXI "develop/blend.h" "DEVELOP_MASK_FLEXI")
