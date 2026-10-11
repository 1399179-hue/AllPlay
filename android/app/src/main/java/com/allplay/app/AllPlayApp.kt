package com.allplay.app

import android.app.Application
import android.view.Surface

/**
 * 进程级单例：让 MainActivity 能把 Surface 交给常驻服务（避免复杂 Binder）。
 */
class AllPlayApp : Application() {
    var service: AllPlaySessionService? = null
    var surface: Surface? = null
}
