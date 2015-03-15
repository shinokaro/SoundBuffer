require "mkmf"

SYSTEM_LIBRARIES = [
  "dxguid",
  "dsound",
  "gdi32",
  "ole32",
  "user32",
  "kernel32",
  "uuid" # for GUID_NULL
]

SYSTEM_LIBRARIES.each do |lib|
  have_library(lib)
end
#have_header("ks.h")
have_header("dsound.h")

create_makefile("soundbuffer")
