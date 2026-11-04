#pragma once

#ifdef VELA_BINDGEN
#define VELA_DETAIL_ANNOTATE(text) [[clang::annotate(text)]]
#else
#define VELA_DETAIL_ANNOTATE(text)
#endif

#define VELA_CLASS(name) VELA_DETAIL_ANNOTATE("vela.class:" name)
#define VELA_NAME(name) VELA_DETAIL_ANNOTATE("vela.name:" name)
#define VELA_GETTER(name) VELA_DETAIL_ANNOTATE("vela.getter:" name)
#define VELA_SETTER(name) VELA_DETAIL_ANNOTATE("vela.setter:" name)
#define VELA_IGNORE VELA_DETAIL_ANNOTATE("vela.ignore")
