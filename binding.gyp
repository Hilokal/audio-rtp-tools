{
  "targets": [
    {
      "target_name": "worker",
      "sources": [
        "src/worker.cc",
        "src/thread_messages.cc",
        "src/node_errors.cc",
        "src/demuxer.cc",
        "src/producer_thread.cc",
        "src/thread_finished_node_callback.cc",
        "src/buffer_ready_node_callback.cc",
        "src/util.cc",
        "src/time_util.cc",
        "src/audio_decode_thread.cc",
        "src/audio_encode_thread.cc"
      ],
      "link_settings": {
        "ldflags": [
          "-Wl,-Bsymbolic"
        ],
        "libraries": [
            "-lavformat",
            "-lavcodec",
            "-lavutil",
            "-lopus",
            "-lswresample",
            # Add these if building with local ffmpeg source
            # "-Foundation",
            # "$(SDKROOT)/System/Library/Frameworks/Cocoa.framework",
            # "$(SDKROOT)/System/Library/Frameworks/QuartzCore.framework",
            # "$(SDKROOT)/System/Library/Frameworks/CoreMedia.framework",
            # "$(SDKROOT)/System/Library/Frameworks/AVFoundation.framework",
        ]
      },
      "conditions": [
        ["OS=='mac'", {
          "library_dirs": [
            "/opt/homebrew/lib"
          ],
          "include_dirs": [
            "/opt/homebrew/include"
          ]
          # "library_dirs": [
          #   "/Users/jon/work/ffmpeg/libavformat",
          #   "/Users/jon/work/ffmpeg/libavcodec",
          #   "/Users/jon/work/ffmpeg/libavutil",
          #   "/opt/homebrew/lib",
          # ],
          # "include_dirs": [
          #   "/Users/jon/work/ffmpeg",
          # ]
        }],
        ["OS=='linux'", {
          "link_settings": {
            "ldflags": [
              "-Wl,-Bsymbolic"
            ]
          }
        }],
      ]
    }
  ]
}
