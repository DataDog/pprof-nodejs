{
  "variables": {
    "sanitizer%": 0, # enable address + undefined behaviour sanitizer
    "thread_sanitizer%": 0, # enable thread sanitizer
  },
  "target_defaults": {
     "conditions": [
        ["OS == 'mac'", {
          "xcode_settings": {
            "OTHER_CFLAGS+": [
              "-Wall",
              "-Werror",
              "-Wno-deprecated-declarations",
            ]
          },
        }],
        ["OS == 'linux'", {
          "cflags+": [
            "-std=gnu++17",
            "-Wall",
            "-Werror"
          ],
          # "cflags_cc": [
          #   "-Wno-cast-function-type",
          #   # TODO: Remove when nan is updated to support v18 properly
          #   "-Wno-deprecated-declarations",
          # ]
        }],
        ["OS == 'win'", {
          "cflags": [
            "/WX"
          ]
        }],
        ['sanitizer == 1 and OS == "mac"', {
          'xcode_settings': {
            'OTHER_CFLAGS+': [
              '-fno-omit-frame-pointer',
              '-fsanitize=address,undefined',
            ],
            'OTHER_CFLAGS!': [
              '-fomit-frame-pointer',
            ],
          },
          'target_conditions': [
            ['_type!="static_library"', {
              'xcode_settings': {'OTHER_LDFLAGS+': ['-fsanitize=address,undefined']},
            }],
          ],
        }],
        ["sanitizer == 1 and OS != 'mac'", {
          "cflags+": [
            "-fno-omit-frame-pointer",
            "-fsanitize=address,undefined",
          ],
          "cflags!": [ "-fomit-frame-pointer" ],
          "ldflags+": [ "-fsanitize=address,undefined" ],
        }],
     ]
  },
  "targets": [
    {
      "target_name": "dd_pprof",
      "sources": [
        "bindings/profilers/cpu.cc",
        "bindings/profilers/heap.cc",
        "bindings/profilers/wall.cc",
        "bindings/code-event-record.cc",
        "bindings/code-map.cc",
        "bindings/cpu-time.cc",
        "bindings/location.cc",
        "bindings/per-isolate-data.cc",
        "bindings/sample.cc",
        "bindings/binding.cc",
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
      ],
    },
    {
      "target_name": "test_dd_pprof",
      "sources": [
        "bindings/profilers/cpu.cc",
        "bindings/profilers/heap.cc",
        "bindings/profilers/wall.cc",
        "bindings/code-event-record.cc",
        "bindings/code-map.cc",
        "bindings/cpu-time.cc",
        "bindings/location.cc",
        "bindings/per-isolate-data.cc",
        "bindings/sample.cc",
        "bindings/test/binding.cc",
        "bindings/test/profilers/cpu.test.cc",
        "bindings/test/code-event-record.test.cc",
        "bindings/test/code-map.test.cc",
        "bindings/test/cpu-time.test.cc",
        "bindings/test/location.test.cc",
        "bindings/test/sample.test.cc",
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
      ],
    },
  ]
}
