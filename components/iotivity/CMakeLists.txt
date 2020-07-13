# Write following file list referencing:
# ==> https://github.com/espressif/esp32-iotivity/blob/master/components/iotivity/component.mk

set(include_dirs adapter/include
  iotivity-constrained
  iotivity-constrained/include
  iotivity-constrained/messaging/coap
  iotivity-constrained/port
  iotivity-constrained/util
  iotivity-constrained/util/pt
  iotivity-constrained/deps/tinycbor/src)

set(srcs 
    "iotivity-constrained/deps/tinycbor/src/cborencoder.c"
    "iotivity-constrained/deps/tinycbor/src/cborencoder_close_container_checked.c"
    "iotivity-constrained/deps/tinycbor/src/cborparser.c"

    "adapter/src/random.c"
    "adapter/src/storage.c"
    "adapter/src/clock.c"
    "adapter/src/ipadapter.c"
    "adapter/src/abort.c"
    "adapter/src/debug_print.c"

    "iotivity-constrained/util/oc_etimer.c"
    "iotivity-constrained/util/oc_list.c"
    "iotivity-constrained/util/oc_memb.c"
    "iotivity-constrained/util/oc_mmem.c"
    "iotivity-constrained/util/oc_process.c"
    "iotivity-constrained/util/oc_timer.c"

    "iotivity-constrained/api/oc_base64.c"
    "iotivity-constrained/api/oc_blockwise.c"
    "iotivity-constrained/api/oc_buffer.c"
    "iotivity-constrained/api/oc_client_api.c"
    "iotivity-constrained/api/oc_collection.c"
    "iotivity-constrained/api/oc_core_res.c"
    "iotivity-constrained/api/oc_discovery.c"
    "iotivity-constrained/api/oc_endpoint.c"
    "iotivity-constrained/api/oc_helpers.c"
    "iotivity-constrained/api/oc_introspection.c"
    "iotivity-constrained/api/oc_main.c"
    "iotivity-constrained/api/oc_network_events.c"
    "iotivity-constrained/api/oc_rep.c"
    "iotivity-constrained/api/oc_ri.c"
    "iotivity-constrained/api/oc_server_api.c"
    "iotivity-constrained/api/oc_uuid.c"

    "iotivity-constrained/messaging/coap/coap.c"
    "iotivity-constrained/messaging/coap/engine.c"
    "iotivity-constrained/messaging/coap/observe.c"
    "iotivity-constrained/messaging/coap/separate.c"
    "iotivity-constrained/messaging/coap/transactions.c"
)

set(COMPONENT_REQUIRES lwip)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "${include_dirs}"
                    REQUIRES lwip)

# Silence format truncation warning, until it is fixed upstream
## set_source_files_properties(libcoap/src/coap_debug.c PROPERTIES COMPILE_FLAGS -Wno-format-truncation)

# Needed for coap headers in public builds, also.
#
# TODO: find a way to move this to a port header
target_compile_definitions(${COMPONENT_LIB} PUBLIC WITH_POSIX)
