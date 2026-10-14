{
  "targets": [
    {
      "target_name": "forkdb",
      "sources": [
        "src/forkdb.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('path').dirname(require.resolve('node-addon-api'))\")",
        "../../../../../../projects/forkdb/store",
        "../../../../../../projects/forkdb/fkx",
        "../../../../../../projects/forkdb/vfs",
        "../../../../../../projects/forkdb/spike/vendor/sqlite-amalgamation-3500000",
        "/home/openclaw/projects/forkdb/spike/vendor/sqlite-amalgamation-3500000"
      ],
      "libraries": [
        "-L../../../../../../projects/forkdb/lib",
        "-lforkdb",
        "-L../../../../../../projects/forkdb/vfs/build",
        "-lsqlite3"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "SQLITE_ENABLE_SESSION",
        "SQLITE_ENABLE_PREUPDATE_HOOK"
      ],
      "conditions": [
        ["OS==\"win\"", {
          "defines": [
            "_HAS_EXCEPTIONS=0"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 0
            }
          }
        }],
        ["OS==\"mac\"", {
          "xcode_settings": {
            "CLANG_CXX_LIBRARY": "libc++",
            "GCC_ENABLE_CPP_EXCEPTIONS": "NO"
          }
        }]
      ]
    }
  ]
}