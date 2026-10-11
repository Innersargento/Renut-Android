package org.renut.android

import android.content.Context
import android.content.res.Configuration
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File
import java.util.Locale

/**
 * The app's language. Chosen in the first-run screen or in Configurações, kept in files/language.txt
 * (a file, not SharedPreferences: the game runs in its own process and must read the current value
 * every time it starts), and applied two ways:
 *  - Compose screens read [code], a state: changing it re-renders them at once, no activity restart
 *    (so the first-run step and a running import survive a language change);
 *  - everything else asks [s], which resolves a string in the chosen language from any thread.
 * Activities also wrap their base context ([attach]) so framework-created text starts out right.
 */
object Language {
    class Option(val code: String, val name: String)

    /** Each language is named in itself, so it can always be found. The first one is the default resources. */
    @JvmField val OPTIONS: List<Option> = listOf(
        Option("pt-BR", "Português (Brasil)"),
        Option("en-US", "English (US)"),
        Option("zh-CN", "中文（普通话）"),
        Option("es", "Español"))

    private var appContext: Context? = null
    private val cache = HashMap<String, Context>()

    /** Observed by Compose; empty until the first [attach] or [current] reads the file. */
    var code by mutableStateOf("")
        private set

    private fun file(context: Context) = File(context.filesDir, "language.txt")

    private fun valid(value: String?) = OPTIONS.firstOrNull { it.code == value }?.code

    /** The saved choice, else the device's language when it is one of ours, else English. */
    @JvmStatic fun current(context: Context): String {
        if (code.isNotEmpty()) return code
        val saved = runCatching { valid(file(context).readText().trim()) }.getOrNull()
        val chosen = saved ?: when (Locale.getDefault().language) {
            "pt" -> "pt-BR"
            "zh" -> "zh-CN"
            "es" -> "es"
            else -> "en-US"
        }
        code = chosen
        return chosen
    }

    /** Saves and applies a language. Safe to call from the UI thread of any screen. */
    @JvmStatic fun set(context: Context, value: String) {
        val chosen = valid(value) ?: return
        runCatching { file(context).writeText(chosen) }
        code = chosen
    }

    /** A context whose resources are in [language]. */
    @JvmStatic fun localized(context: Context, language: String): Context {
        val base = context.applicationContext ?: context
        synchronized(cache) {
            return cache.getOrPut(language) {
                val configuration = Configuration(base.resources.configuration)
                configuration.setLocale(Locale.forLanguageTag(language))
                base.createConfigurationContext(configuration)
            }
        }
    }

    /**
     * Resources in [language] with [live] as their configuration. A context made by
     * createConfigurationContext keeps the configuration it was made with, so a screen that
     * is not recreated on rotation has to be given the current one here.
     */
    @JvmStatic fun derive(context: Context, live: Configuration, language: String): Context {
        val configuration = Configuration(live)
        configuration.setLocale(Locale.forLanguageTag(language))
        return context.createConfigurationContext(configuration)
    }

    /**
     * A copy of [live] with the language's locale. The system changes the configuration of the
     * resources in place when the screen rotates, so a copy is taken at every use: a cached one
     * (or the same instance used as a cache key) would keep the orientation it was taken at.
     */
    @JvmStatic fun configuration(live: Configuration, language: String): Configuration {
        val configuration = Configuration(live)
        configuration.setLocale(Locale.forLanguageTag(language))
        return configuration
    }

    /** For attachBaseContext: remembers the app context and returns the base in the chosen language. */
    @JvmStatic fun attach(base: Context): Context {
        appContext = base.applicationContext ?: base
        val configuration = Configuration(base.resources.configuration)
        configuration.setLocale(Locale.forLanguageTag(current(base)))
        return base.createConfigurationContext(configuration)
    }

    /** A string in the chosen language; usable from any thread once an activity or service attached. */
    @JvmStatic fun s(id: Int, vararg args: Any?): String {
        val context = appContext ?: return ""
        return localized(context, current(context)).getString(id, *args)
    }

    /** A string array (the controller labels) in the chosen language. */
    @JvmStatic fun array(id: Int): Array<String> {
        val context = appContext ?: return emptyArray()
        return localized(context, current(context)).resources.getStringArray(id)
    }

    /** The locale for dates and numbers. */
    @JvmStatic fun locale(context: Context): Locale = Locale.forLanguageTag(current(context))
}
