/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/xbox.h"
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

#include <sstream>      // std::stringstream
#include <third_party/libcurl/include/curl/curl.h>
#include <third_party/rapidjson/include/rapidjson/document.h>

#include <WinSock2.h>
#include "third_party/fmt/include/fmt/format.h"
#include <src/xenia/kernel/xam/xam_net.h>

namespace xe {
namespace kernel {
namespace xam {

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, lpvoid_t buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned,
                        uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr, overlapped_ptr](uint32_t& extended_error,
                                             uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(),
                             buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

dword_result_t XamEnumerate_entry(dword_t handle, dword_t flags,
                                  lpvoid_t buffer, dword_t buffer_length,
                                  lpdword_t items_returned,
                                  pointer_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result = xeXamEnumerate(handle, flags, buffer, buffer_length,
                               !overlapped ? &dummy : nullptr, overlapped);
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamEnumerate, kNone, kImplemented);

struct XTitleServer {
  in_addr server_address;
  DWORD flags;
  CHAR server_description[200];
};

std::size_t callback(const char* in, std::size_t size, std::size_t num, char* out) {
  std::string data(in, (std::size_t)size * num);
  *((std::stringstream*)out) << data;
  return size * num;
} 

dword_result_t XamCreateEnumeratorHandle_entry(
    dword_t user_index, dword_t app_id, dword_t open_message,
    dword_t close_message, dword_t extra_size, dword_t item_count,
    dword_t flags, lpdword_t out_handle) {
  // if the message is the LSP create enum message...
  if (open_message == 0x58039) {
    auto e = make_object<XStaticEnumerator<XTitleServer>>(kernel_state(),
                                                                item_count);
    auto result =
        e->Initialize(user_index, app_id, open_message, close_message, flags);

    if (XFAILED(result)) {
      return result;
    }

#pragma region Curl
    /*
        TODO:
            - Refactor the CURL out to a separate class.
            - Implement an XEnumerator that supports asyncronous enumeration.
    */

    CURL* curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl == NULL) {
      return 128;
    }

    std::stringstream out;

    struct curl_slist* headers = NULL;
    curl_slist_append(headers, "Accept: application/json");
    curl_slist_append(headers, "Content-Type: application/json");
    curl_slist_append(headers, "charset: utf-8");

    std::stringstream titleId;
    titleId << std::hex << std::noshowbase << std::setw(8) << std::setfill('0')
            << kernel_state()->title_id();

    std::stringstream url;
    url << GetApiAddress() << "/title/" << titleId.str() << "/servers";

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);

    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    int httpCode(0);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    // curl_global_cleanup();

    if (httpCode == 200) {
      rapidjson::Document d;
      d.Parse(out.str().c_str());

      for (const auto& server : d.GetArray()) {
        auto item = e->AppendItem();
        item->server_address.S_un.S_addr =
            inet_addr(server["address"].GetString());
        item->flags = server["flags"].GetInt();
        memcpy(item->server_description, server["description"].GetString(),
               strlen(server["description"].GetString()));
      }
    } else {
        return 1;
    }

#pragma endregion

    XELOGI("XamCreateEnumerator: added {} items to enumerator",
           e->item_count());

    *out_handle = e->handle();
    return X_ERROR_SUCCESS;
  } else {
    auto e = object_ref<XStaticUntypedEnumerator>(
        new XStaticUntypedEnumerator(kernel_state(), item_count, extra_size));

    auto result =
        e->Initialize(user_index, app_id, open_message, close_message, flags);
    if (XFAILED(result)) {
      return result;
    }

    *out_handle = e->handle();
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamCreateEnumeratorHandle, kNone, kImplemented);

dword_result_t XamGetPrivateEnumStructureFromHandle_entry(
    dword_t handle, lpdword_t out_object_ptr) {
  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetPrivateEnumStructureFromHandle, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Enum);
