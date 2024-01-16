{
    "variables": {
        "address_sanitizer%": 0, # enable address + undefined behaviour sanitizer
        "thread_sanitizer%": 0, # enable thread sanitizer,
        "build_tests%": 0
    },
    "conditions": [
        [
            "build_tests != 'true'",
            {
                "targets": [
                    {
                        "target_name": "dd_pprof",
                        "sources": [
                            "bindings/profilers/heap.cc",
                            "bindings/profilers/wall.cc",
                            "bindings/per-isolate-data.cc",
                            "bindings/thread-cpu-clock.cc",
                            "bindings/translate-time-profile.cc",
                            "bindings/binding.cc"
                        ],
                        "include_dirs": [
                            "bindings",
                            "<!(node -e \"require('nan')\")"
                        ]
                    }
                ]
            }
        ],
        [
            "build_tests == 'true'",
            {
                "targets": [
                    {
                        "target_name": "test_dd_pprof",
                        "sources": [
                            "bindings/profilers/heap.cc",
                            "bindings/profilers/wall.cc",
                            "bindings/per-isolate-data.cc",
                            "bindings/thread-cpu-clock.cc",
                            "bindings/translate-time-profile.cc",
                            "bindings/test/binding.cc",
                        ],
                        "include_dirs": [
                            "bindings",
                            "<!(node -e \"require('nan')\")"
                        ]
                    }
                ]
            }
        ]
    ],
    "target_defaults": {
        "conditions": [
            [
                'OS == "win"',
                {
                    "defines": [
                        "NOMINMAX"
                    ],
                }
            ],
            ["address_sanitizer == 'true' and OS == 'mac'", {
                'xcode_settings': {
                    'OTHER_CFLAGS+': [
                        '-fno-omit-frame-pointer',
                        '-fsanitize=address,undefined',
                        '-O0',
                        '-g',
                    ],
                    'OTHER_CFLAGS!': [
                        '-fomit-frame-pointer',
                        '-O3',
                    ],
                },
                'target_conditions': [
                    ['_type!="static_library"', {
                        'xcode_settings': {'OTHER_LDFLAGS+': ['-fsanitize=address,undefined']},
                    }],
                ],
            }],
            ["address_sanitizer == 'true' and OS != 'mac'", {
                "cflags+": [
                "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined",
                "-O0",
                "-g",
                ],
                "cflags!": [ "-fomit-frame-pointer", "-O3" ],
                "ldflags+": [ "-fsanitize=address,undefined" ],
            }],
            ["thread_sanitizer == 'true' and OS == 'mac'", {
                'xcode_settings': {
                    'OTHER_CFLAGS+': [
                        '-fno-omit-frame-pointer',
                        '-fsanitize=thread',
                    ],
                    'OTHER_CFLAGS!': [
                        '-fomit-frame-pointer',
                    ],
                },
                'target_conditions': [
                    ['_type!="static_library"', {
                        'xcode_settings': {'OTHER_LDFLAGS+': ['-fsanitize=thread']},
                    }],
                ],
            }],
            ["thread_sanitizer == 'true' and OS != 'mac'", {
                "cflags+": [
                "-fno-omit-frame-pointer",
                "-fsanitize=thread",
                ],
                "cflags!": [ "-fomit-frame-pointer" ],
                "ldflags+": [ "-fsanitize=thread" ],
            }]
        ],
    }
}
