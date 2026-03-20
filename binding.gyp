{
  "targets": [{
    "target_name": "patcher",
    "sources": ["patcher.cpp"],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")"
    ],
    "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
    "conditions": [
      ["OS=='win'", {
        "libraries": ["Psapi.lib"],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": 0,
            "AdditionalOptions": ["/std:c++17"]
          }
        }
      }]
    ]
  }]
}