"""Standalone C embedding through generated exports and shared local/persistent roots."""

HOST = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "TEXT_HEADER"

int main(void) {
  VelaAotIsolate* isolate = vela_aot_v2_isolate_create();
  assert(isolate != NULL);
  VelaAotScope* scope = NULL;
  assert(vela_aot_v2_scope_create(isolate, &scope) == VELA_AOT_RETURNED);
  VelaAotInstance* instance = NULL;
  assert(TEXT_INSTANTIATE(isolate, &instance, NULL) == VELA_AOT_RETURNED);
  VelaAotLocal input = {0};
  assert(vela_aot_v2_string_create(scope, "hello", 5, &input) == VELA_AOT_RETURNED);
  VelaAotValue argument = {.kind = VELA_AOT_LOCAL, .local = input};
  VelaAotValue output = {0};
  assert(TEXT_CALL(scope, instance, &argument, 1, &output, NULL) == VELA_AOT_RETURNED);
  assert(output.kind == VELA_AOT_LOCAL);
  VelaAotPersistent* saved = NULL;
  assert(vela_aot_v2_persist(output.local, &saved) == VELA_AOT_RETURNED);
  vela_aot_v2_instance_release(instance);
  vela_aot_v2_scope_release(scope);
  for (int index = 0; index < 256; ++index) {
    assert(vela_aot_v2_scope_create(isolate, &scope) == VELA_AOT_RETURNED);
    VelaAotLocal temporary = {0};
    assert(vela_aot_v2_string_create(scope, "temporary allocation", 20, &temporary)
           == VELA_AOT_RETURNED);
    vela_aot_v2_scope_release(scope);
  }
  assert(vela_aot_v2_scope_create(isolate, &scope) == VELA_AOT_RETURNED);
  VelaAotLocal text = {0};
  assert(vela_aot_v2_localize(scope, saved, &text) == VELA_AOT_RETURNED);
  vela_aot_v2_persistent_release(saved);
  const char* bytes = NULL;
  uint64_t size = 0;
  assert(vela_aot_v2_string_view(text, &bytes, &size) == VELA_AOT_RETURNED);
  assert(size == 6 && memcmp(bytes, "hello!", 6) == 0);
  vela_aot_v2_scope_release(scope);
  vela_aot_v2_isolate_release(isolate);
  puts("C host passed");
  return 0;
}
'''


def host_source(header: str, manifest: dict) -> str:
    entry = next(value["symbol"] for value in manifest["exports"] if value["name"] == "calculate")
    return (HOST.replace("TEXT_HEADER", header)
            .replace("TEXT_INSTANTIATE", manifest["instantiate"])
            .replace("TEXT_CALL", entry))
