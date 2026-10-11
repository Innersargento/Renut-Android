package org.renut.android

import android.app.Activity
import android.graphics.BitmapFactory
import android.view.SurfaceView
import android.view.View
import androidx.activity.ComponentActivity
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.ExitToApp
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.Speed
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

private val Background = Color(0xFF101113)
private val Surface = Color(0xFF1C1E21)
private val Accent = Color(0xFFFF9B45)
private val Text = Color(0xFFF4F0EB)
private val Muted = Color(0xFFA8A29C)

object ComposeScreens {
    @JvmStatic fun showMain(activity: MainActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { MainScreen(activity) } } })
    }
    @JvmStatic fun showController(activity: ControllerSettingsActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { ControllerScreen(activity) } } })
    }
    @JvmStatic fun showProbe(activity: ProbeActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { ProbeScreen(activity) } } })
    }
    @JvmStatic fun showRuntime(activity: RuntimeActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { RuntimeScreen(activity) } } })
    }
    @JvmStatic fun createGameMenu(activity: RuntimeActivity): GameMenu = GameMenu(activity)
}

/**
 * The in-game sidebar. The view stays on screen for the whole game and only draws what is
 * open (a small tab, the sidebar, or the paused chip), so touches elsewhere reach the game.
 * RuntimeActivity (Java) drives it; Compose observes the state below.
 */
class GameMenu internal constructor(private val activity: RuntimeActivity) {
    internal enum class Page { Main, Achievements }

    var isOpen by mutableStateOf(false); private set
    var isPaused by mutableStateOf(false); private set
    internal var page by mutableStateOf(Page.Main)

    val view: View = ComposeView(activity).apply {
        setContent { RenutTheme { GameMenuScreen(activity, this@GameMenu) } }
    }

    fun open() { page = Page.Main; isOpen = true }
    fun close() { isOpen = false }

    /** Back inside the sidebar: from a sub-page to the list, then closes. True if it was open. */
    fun back(): Boolean {
        if (!isOpen) return false
        if (page != Page.Main) page = Page.Main else close()
        return true
    }

    /** Asks the game to stop (or go on); the state only changes if the game took it. */
    fun requestPaused(paused: Boolean) {
        if (activity.uiSetGamePaused(paused)) isPaused = paused
    }
}

@Composable private fun RenutTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Accent, onPrimary = Background, background = Background,
            surface = Surface, onSurface = Text, onBackground = Text,
            secondary = Accent, secondaryContainer = Color(0xFF4B301F),
            onSecondaryContainer = Accent, surfaceVariant = Surface,
            onSurfaceVariant = Muted, outline = Color(0xFF3A3B3D)
        ), content = content
    )
}

@Composable private fun MainScreen(activity: MainActivity) {
    var page by remember { mutableIntStateOf(0) }
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(700); tick++ } }
    Scaffold(
        containerColor = Background,
        bottomBar = {
            NavigationBar(containerColor = Surface) {
                NavigationBarItem(selected = page == 0, onClick = { page = 0 },
                    icon = { Icon(Icons.Default.PlayArrow, null) }, label = { Text("Jogar") })
                NavigationBarItem(selected = page == 1, onClick = { page = 1 },
                    icon = { Icon(Icons.Default.Folder, null) }, label = { Text("Saves") })
                NavigationBarItem(selected = page == 2, onClick = { page = 2 },
                    icon = { Icon(Icons.Default.EmojiEvents, null) }, label = { Text("Conquistas") })
                NavigationBarItem(selected = page == 3, onClick = { page = 3 },
                    icon = { Icon(Icons.Default.Settings, null) }, label = { Text("Configurações") })
            }
        }
    ) { padding ->
        when (page) {
            0 -> PlayScreen(activity, Modifier.padding(padding), tick) { page = 2 }
            1 -> SaveScreen(activity, Modifier.padding(padding), tick)
            2 -> AchievementsScreen(activity, Modifier.padding(padding), tick)
            else -> SettingsScreen(activity, Modifier.padding(padding), tick)
        }
    }
}

@Composable private fun SaveScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val saves = rememberUpdatedState(runCatching { activity.uiSaves() }.getOrDefault(emptyList()))
    val visibleSaves = saves.value.filterNot { it.contains("/Headers/") }.ifEmpty { saves.value }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("SEUS SAVES", "Arquivos do jogo", "Gerencie os saves guardados neste aparelho.") }
        item {
            Panel("Biblioteca") {
                Text(if (visibleSaves.isEmpty()) "Nenhum save encontrado" else "${visibleSaves.size} save(s) encontrado(s)", color = Text, fontWeight = FontWeight.SemiBold)
                Text("Os arquivos são lidos da pasta usada pelo jogo.", color = Muted, fontSize = 13.sp)
            }
        }
        if (visibleSaves.isNotEmpty()) {
            item { SectionTitle("Arquivos") }
            items(visibleSaves) { name -> Panel(null) {
                Text(name.substringAfterLast('/'), color = Text, fontWeight = FontWeight.SemiBold)
                Text("Save do jogo", color = Muted, fontSize = 13.sp)
            } }
        }
        item {
            Panel("Transferir") {
                ActionButton("Exportar saves (.zip)", !activity.uiBusy()) { activity.uiAction("Exportar saves (.zip)") }
                ActionButton("Importar saves (.zip)", !activity.uiBusy(), outlined = true) { activity.uiAction("Importar saves (.zip)") }
                Text("A importação pede confirmação antes de substituir arquivos existentes.", color = Muted, fontSize = 12.sp)
            }
        }
    }
}

@Composable private fun PlayScreen(activity: MainActivity, modifier: Modifier, tick: Int, openAchievements: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val state = activity.uiGameState()
    val busy = activity.uiBusy()
    val autoStart = activity.uiAutoStart()
    val stamp = Achievements.stamp(activity.filesDir)
    val progress by produceState<Achievements.Snapshot?>(null, stamp) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
    }
    // Read from the game on its first launch; until then the card shows a placeholder.
    val cover = remember(stamp) {
        runCatching { BitmapFactory.decodeFile(java.io.File(activity.filesDir, "achievements/title.png").path)?.asImageBitmap() }.getOrNull()
    }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("RENUT / JOGAR", "Pronto para jogar?", "Sua aventura continua de onde você parou.") }
        item {
            Card(colors = CardDefaults.cardColors(containerColor = Color.Transparent), shape = RoundedCornerShape(24.dp), modifier = Modifier.fillMaxWidth()) {
                Column(
                    Modifier.background(Brush.linearGradient(listOf(Color(0xFF4B301F), Color(0xFF241A14), Surface))).padding(20.dp),
                    verticalArrangement = Arrangement.spacedBy(18.dp)
                ) {
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(84.dp).clip(RoundedCornerShape(18.dp)).background(Color(0x33FF9B45)), contentAlignment = Alignment.Center) {
                            if (cover != null) Image(cover, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
                            else Icon(Icons.Default.PlayArrow, null, tint = Accent, modifier = Modifier.size(40.dp))
                        }
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("Banjo-Kazooie", color = Text, fontWeight = FontWeight.Bold, fontSize = 24.sp)
                            Text("Nuts & Bolts", color = Accent, fontWeight = FontWeight.SemiBold, fontSize = 17.sp)
                            Text(when (state) {
                                2 -> "Edição europeia · jogo completo"
                                1 -> "Edição europeia · só o XEX importado"
                                else -> "Nenhum arquivo importado"
                            }, color = Muted, fontSize = 13.sp)
                        }
                    }
                    Button(
                        onClick = { activity.uiStartGame() }, enabled = state == 2 && !busy,
                        modifier = Modifier.fillMaxWidth().height(60.dp), shape = RoundedCornerShape(16.dp)
                    ) {
                        Icon(Icons.Default.PlayArrow, null, modifier = Modifier.size(28.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("Jogar", fontWeight = FontWeight.Bold, fontSize = 19.sp)
                    }
                    if (state != 2) Text(
                        if (state == 1) "Importe a imagem ISO completa para ter os arquivos que o jogo precisa."
                        else "Importe a imagem ISO do jogo abaixo para começar.",
                        color = Muted, fontSize = 13.sp)
                    else Text("Experimental: o desempenho e a compatibilidade ainda estão em desenvolvimento.", color = Muted, fontSize = 12.sp)
                    if (state == 2 && activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true")
                        Text("Cheats ligados nesta partida", color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }
        progress?.let { data ->
            item {
                Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp),
                    modifier = Modifier.fillMaxWidth().clickable { openAchievements() }) {
                    Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.EmojiEvents, null, tint = Accent, modifier = Modifier.size(32.dp))
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text("${data.unlocked.size} de ${data.count()} conquistas", color = Text, fontWeight = FontWeight.SemiBold)
                            LinearProgressIndicator(
                                progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                                modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                                color = Accent, trackColor = Color(0xFF3A3B3D))
                        }
                        Text("${data.earnedGamerscore} G", color = Accent, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
        item { CheatsPanel(activity, !busy) }
        item {
            Panel("Ao abrir o app") {
                ToggleRow("Iniciar direto no jogo", autoStart, !busy) { activity.uiSetAutoStart(it) }
                Text("O jogo começa sem passar por esta tela. Para voltar aqui, saia do jogo pelo menu.", color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel("Arquivos do jogo") {
                ActionButton("Importar imagem ISO", !busy, outlined = true) { activity.uiAction("Importar imagem ISO") }
                ActionButton("Importar apenas o XEX", !busy, outlined = true) { activity.uiAction("Importar apenas o XEX") }
                if (busy) Text(activity.uiStatus(), color = Muted, fontSize = 13.sp, maxLines = 4, overflow = TextOverflow.Ellipsis)
                if (activity.uiCanCancel()) ActionButton("Cancelar importação", true, outlined = true) { activity.uiCancelImport() }
                Text("A importação da ISO precisa de espaço livre para a imagem e os arquivos extraídos.", color = Muted, fontSize = 12.sp)
            }
        }
    }
}

private val CHEAT_LABELS = listOf(
    "renut_cheat_infinite_health" to "Vida infinita",
    "renut_cheat_infinite_fuel_ammo" to "Combustível e munição infinitos",
    "renut_cheat_infinite_parts" to "Peças infinitas",
    "renut_cheat_no_timer" to "Sem limite de tempo nas missões",
    "renut_cheat_no_notes_spent" to "Notas não são gastas",
    "renut_cheat_vehicles_anywhere" to "Trocar e montar veículo em qualquer lugar",
    "renut_cheat_extended_build_range" to "Área de montagem maior (Mumbo's Motors)")
private val SKIN_LABELS = listOf("default" to "Normal", "robot" to "Robô", "tuxedo" to "Smoking")

/** Cheats of the next game start. The switches live in renut.toml, which the runtime reads at launch. */
@Composable private fun CheatsPanel(activity: MainActivity, enabled: Boolean) {
    var master by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true") }
    val flags = remember {
        mutableStateMapOf<String, Boolean>().apply {
            CHEAT_LABELS.forEach { (key, _) -> put(key, activity.uiRuntimeOption(key, "false") == "true") }
        }
    }
    var skin by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEAT_SKIN, "default")) }
    Panel("Cheats") {
        ToggleRow("Aplicar cheats ao jogar", master, enabled) {
            master = it
            activity.uiSetRuntimeOption(RuntimeOptions.CHEATS, it.toString())
        }
        if (master) {
            CHEAT_LABELS.forEach { (key, label) ->
                ToggleRow(label, flags[key] == true, enabled) {
                    flags[key] = it
                    activity.uiSetRuntimeOption(key, it.toString())
                }
            }
            SingleChoiceSelector("Visual do Banjo", SKIN_LABELS.firstOrNull { it.first == skin }?.second ?: "Normal",
                SKIN_LABELS.map { it.second }) { label ->
                val value = SKIN_LABELS.first { it.second == label }.first
                skin = value
                activity.uiSetRuntimeOption(RuntimeOptions.CHEAT_SKIN, value)
            }
        }
        Text("Valem a partir da próxima partida. Peças infinitas altera as contagens de peças do seu save.", color = Muted, fontSize = 12.sp)
    }
}

@Composable private fun AchievementsScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    // The runtime rewrites these files while the game runs; reload only when they change.
    @Suppress("UNUSED_VARIABLE") val update = tick
    val stamp = Achievements.stamp(activity.filesDir)
    val loaded = remember { mutableStateOf(false) }
    val snapshot by produceState<Achievements.Snapshot?>(null, stamp) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
        loaded.value = true
    }
    val data = snapshot
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("CONQUISTAS", "Suas conquistas", "As conquistas desbloqueadas no jogo, neste aparelho.") }
        if (data == null) {
            if (loaded.value) item {
                Panel("Ainda sem lista") {
                    Text("A lista de conquistas é lida do seu jogo na primeira vez em que ele inicia. Abra o jogo uma vez e volte aqui.", color = Muted, fontSize = 14.sp)
                }
            }
        } else {
            item {
                Panel("Progresso") {
                    Text("${data.unlocked.size} de ${data.count()} desbloqueadas", color = Text, fontWeight = FontWeight.SemiBold)
                    LinearProgressIndicator(
                        progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                        modifier = Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                        color = Accent, trackColor = Color(0xFF3A3B3D))
                    Text("${data.earnedGamerscore} de ${data.gamerscore} G", color = Muted, fontSize = 13.sp)
                }
            }
            if (data.unlocked.isNotEmpty()) {
                item { SectionTitle("Desbloqueadas") }
                items(data.unlocked, key = { it.id }) { AchievementRow(it) }
            }
            if (data.locked.isNotEmpty()) {
                item { SectionTitle("Bloqueadas") }
                items(data.locked, key = { it.id }) { AchievementRow(it) }
            }
        }
    }
}

@Composable private fun AchievementRow(item: Achievements.Item, compact: Boolean = false) {
    val unlocked = item.unlocked()
    val icon = remember(item.icon) { item.icon?.let { runCatching { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }.getOrNull() } }
    val gray = remember { ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }) }
    val pad = if (compact) 10.dp else 14.dp
    Card(colors = CardDefaults.cardColors(containerColor = if (compact) Color(0xFF26282B) else Surface),
        shape = RoundedCornerShape(if (compact) 12.dp else 16.dp), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(pad), horizontalArrangement = Arrangement.spacedBy(pad), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(if (compact) 42.dp else 56.dp).clip(RoundedCornerShape(if (compact) 9.dp else 12.dp))
                .background(Color(0xFF34363A)).alpha(if (unlocked) 1f else 0.45f)) {
                if (icon != null) Image(icon, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop, colorFilter = if (unlocked) null else gray)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(item.label, color = if (unlocked) Text else Muted, fontWeight = FontWeight.Bold, fontSize = if (compact) 14.sp else 16.sp)
                Text(item.shownDescription(), color = Muted, fontSize = if (compact) 12.sp else 13.sp)
                if (unlocked) Text(
                    "Desbloqueada em " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(item.unlockedAt)),
                    color = Accent, fontSize = if (compact) 11.sp else 12.sp)
            }
            if (item.gamerscore > 0) Text("${item.gamerscore} G", color = if (unlocked) Accent else Muted,
                fontWeight = FontWeight.Bold, fontSize = if (compact) 13.sp else 14.sp)
        }
    }
}

@Composable private fun SettingsScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val driver = activity.uiDriverSelected()
    var confirmDeleteDriver by remember { mutableStateOf(false) }
    if (confirmDeleteDriver) AlertDialog(
        onDismissRequest = { confirmDeleteDriver = false },
        title = { Text("Apagar driver?") },
        text = { Text("\"$driver\" será removido do aparelho e o driver do sistema passará a ser usado. Você poderá importá-lo de novo.") },
        confirmButton = { TextButton(onClick = { confirmDeleteDriver = false; activity.uiDeleteDriver() }) { Text("Apagar") } },
        dismissButton = { TextButton(onClick = { confirmDeleteDriver = false }) { Text("Cancelar") } })
    val busy = activity.uiBusy()
    val fpsEnabled = activity.uiFpsEnabled()
    val controlsEnabled = activity.uiShowControls()
    val oneUiEnabled = activity.uiOneUiEnabled()
    val tuMode = activity.uiTuMode()
    val ir3Mode = activity.uiIr3Mode()
    val turnip = remember(driver) { activity.uiDriverIsTurnip() }
    val logLevel = activity.uiRuntimeOption("log_level", "info")
    val shaderAsync = activity.uiRuntimeOption("renut_plume_async_pipelines", "true") == "true"
    val uncapFps = activity.uiRuntimeOption("renut_plume_uncap_fps", "false") == "true"
    val noShadows = activity.uiRuntimeOption("renut_plume_no_shadows", "false") == "true"
    val noBloom = activity.uiRuntimeOption("renut_plume_no_bloom", "false") == "true"
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("RENUT / ANDROID", "Configurações", "Jogo, vídeo, controles e diagnóstico.") }
        item {
            Panel("Display") {
                ToggleRow("Contador de FPS", fpsEnabled, !busy) { activity.uiSetFps(it) }
                ToggleRow("Destravar FPS", uncapFps, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_uncap_fps", it.toString())
                }
                Text("Usa o modo de apresentação imediato e a maior frequência da tela na próxima partida. Pode alterar o ritmo de partes do jogo.", color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel("Controles") {
                ToggleRow("Mostrar controles na tela", controlsEnabled, !busy) { activity.uiSetShowControls(it) }
                ActionButton("Configurar controle externo", !busy, outlined = true) { activity.uiAction("Configurar controle externo") }
            }
        }
        item {
            Panel("Avançado") {
                SingleChoiceSelector("Driver", driver, activity.uiDriverChoices(), !busy) { activity.uiSelectDriver(it) }
                ActionButton("Importar driver Adreno (.zip)", !busy, outlined = true) { activity.uiAction("Importar driver Adreno (.zip)") }
                if (activity.uiDriverStored()) ActionButton("Apagar driver selecionado", !busy, outlined = true) { confirmDeleteDriver = true }
                if (turnip) {
                    ToggleRow("One UI fix", oneUiEnabled, !busy) { activity.uiSetOneUi(it) }
                    MultiModeSelector("TU_DEBUG", tuMode, listOf("perf", "sysmem", "gmem")) { activity.uiSetTuMode(it) }
                    MultiModeSelector("IR3_SHADER_DEBUG", ir3Mode, listOf("nopreamble", "noearlypreamble", "nouboopt", "nodescprefetch")) { activity.uiSetIr3Mode(it) }
                } else {
                    Text("Opções do Adreno (One UI fix, TU_DEBUG, IR3_SHADER_DEBUG) aparecem ao selecionar um driver Turnip.", color = Muted, fontSize = 12.sp)
                }
                ActionButton("Apagar cache de shaders", !busy, outlined = true) { activity.uiAction("Apagar cache de shaders") }
            }
        }
        item {
            Panel("Opções do runtime") {
                Text("Aplicadas na próxima inicialização do jogo.", color = Muted, fontSize = 13.sp)
                SingleChoiceSelector("Nível do log", logLevel,
                    listOf("trace", "debug", "info", "warn", "error", "critical", "off")) {
                    activity.uiSetRuntimeOption("log_level", it)
                }
                ToggleRow("Compilação assíncrona de shaders e pipelines", shaderAsync, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_async_pipelines", it.toString())
                }
                ToggleRow("Remover sombras", noShadows, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_no_shadows", it.toString())
                }
                ToggleRow("Remover bloom", noBloom, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_no_bloom", it.toString())
                }
            }
        }
        item { Text("reNut · prévia de desenvolvimento", color = Muted, fontSize = 12.sp) }
    }
}

@Composable private fun Header(eyebrow: String, title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(eyebrow, color = Accent, fontWeight = FontWeight.Bold, fontSize = 12.sp, letterSpacing = 1.sp)
        Text(title, color = Text, fontWeight = FontWeight.Bold, fontSize = 32.sp)
        Text(subtitle, color = Muted, fontSize = 14.sp)
    }
}

@Composable private fun SectionTitle(title: String) { Text(title, color = Accent, fontWeight = FontWeight.Bold, fontSize = 17.sp) }

@Composable private fun Panel(title: String?, content: @Composable ColumnScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (title != null) Text(title, color = Text, fontWeight = FontWeight.Bold, fontSize = 19.sp)
            content()
        }
    }
}

@Composable private fun InfoRow(label: String, value: String) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(label, color = Accent, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        Text(value, color = Text, fontSize = 14.sp)
    }
}

@Composable private fun ToggleRow(label: String, checked: Boolean, enabled: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Text, modifier = Modifier.weight(1f))
        Switch(checked = checked, enabled = enabled, onCheckedChange = onCheckedChange)
    }
}

@Composable private fun MultiModeSelector(label: String, selected: String, choices: List<String>, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    var local by remember { mutableStateOf(selected) }
    Box {
        OutlinedButton(onClick = { local = selected; expanded = true }, modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(12.dp)) {
            Text("$label: ${(if (expanded) local else selected).ifEmpty { "Desativado" }.replace(",", ", ")}")
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            DropdownMenuItem(text = { Text("Desativado") }, leadingIcon = {
                Checkbox(checked = local.isEmpty(), onCheckedChange = null)
            }, onClick = { local = ""; onSelect("") })
            choices.forEach { choice ->
                DropdownMenuItem(text = { Text(choice) }, leadingIcon = {
                    Checkbox(checked = choice in local.split(',').filter { it.isNotEmpty() }, onCheckedChange = null)
                }, onClick = {
                    val values = local.split(',').filter { it.isNotEmpty() }.toMutableSet()
                    if (!values.add(choice)) values.remove(choice)
                    val updated = choices.filter { it in values }.joinToString(",")
                    local = updated
                    onSelect(updated)
                })
            }
        }
    }
}

@Composable private fun SingleChoiceSelector(label: String, selected: String, choices: List<String>, enabled: Boolean = true, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { expanded = true }, enabled = enabled, modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(12.dp)) {
            Text("$label: $selected")
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            choices.forEach { choice ->
                DropdownMenuItem(text = { Text(choice) }, onClick = { expanded = false; onSelect(choice) })
            }
        }
    }
}

@Composable private fun ActionButton(label: String, enabled: Boolean, outlined: Boolean = false, onClick: () -> Unit) {
    if (outlined) OutlinedButton(onClick, Modifier.fillMaxWidth(), enabled = enabled, shape = RoundedCornerShape(12.dp)) { Text(label) }
    else Button(onClick, Modifier.fillMaxWidth(), enabled = enabled, shape = RoundedCornerShape(12.dp)) { Text(label) }
}

private val SidebarWidth = 360.dp

@Composable private fun GameMenuScreen(activity: RuntimeActivity, menu: GameMenu) {
    val noRipple = remember { MutableInteractionSource() }
    Box(Modifier.fillMaxSize()) {
        // Closed: only a small tab, or the paused chip, sits over the game.
        if (!menu.isOpen) {
            if (menu.isPaused) PausedChip(menu, Modifier.align(Alignment.TopCenter))
            else MenuTab(menu, Modifier.align(Alignment.TopCenter))
        }
        AnimatedVisibility(visible = menu.isOpen, enter = fadeIn(), exit = fadeOut()) {
            Box(Modifier.fillMaxSize().background(Color(0x99000000))
                .clickable(interactionSource = noRipple, indication = null) { menu.close() })
        }
        AnimatedVisibility(
            visible = menu.isOpen, modifier = Modifier.align(Alignment.CenterStart),
            enter = slideInHorizontally { -it }, exit = slideOutHorizontally { -it }
        ) {
            // Flush with the left edge and as tall as the screen; the camera cutout only
            // pushes the content in, the panel itself still reaches the edge.
            Sidebar(activity, menu, Modifier.fillMaxHeight().width(SidebarWidth + cutoutStart())
                .background(Color(0xF21C1E21))
                // The sidebar swallows taps on its empty areas so they do not reach the scrim.
                .clickable(interactionSource = noRipple, indication = null) { }
                .windowInsetsPadding(WindowInsets.displayCutout))
        }
    }
}

/** The way in for touch players; Back (system) opens it as well. Low contrast on purpose. */
/** Width of the camera cutout on the left edge, so the panel keeps its width past it. */
@Composable private fun cutoutStart(): androidx.compose.ui.unit.Dp =
    WindowInsets.displayCutout.asPaddingValues().calculateLeftPadding(androidx.compose.ui.unit.LayoutDirection.Ltr)

@Composable private fun MenuTab(menu: GameMenu, modifier: Modifier) {
    Box(
        modifier.size(width = 64.dp, height = 26.dp)
            .clip(RoundedCornerShape(bottomStart = 14.dp, bottomEnd = 14.dp))
            .background(Color(0x66000000)).clickable { menu.open() },
        contentAlignment = Alignment.Center
    ) { Icon(Icons.Default.Menu, "Abrir menu", tint = Color(0xB3FFFFFF), modifier = Modifier.size(18.dp)) }
}

@Composable private fun PausedChip(menu: GameMenu, modifier: Modifier) {
    Row(
        modifier.clip(RoundedCornerShape(bottomStart = 18.dp, bottomEnd = 18.dp)).background(Color(0xE61C1E21))
            .padding(horizontal = 14.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(Icons.Default.Pause, null, tint = Accent, modifier = Modifier.size(18.dp))
        Text("Pausado", color = Text, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
        Button(onClick = { menu.requestPaused(false) }, contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
            modifier = Modifier.height(32.dp), shape = RoundedCornerShape(10.dp)) {
            Icon(Icons.Default.PlayArrow, null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(4.dp))
            Text("Retomar", fontSize = 13.sp)
        }
        IconButton(onClick = { menu.open() }, modifier = Modifier.size(32.dp)) {
            Icon(Icons.Default.Menu, "Abrir menu", tint = Muted, modifier = Modifier.size(20.dp))
        }
    }
}

@Composable private fun Sidebar(activity: RuntimeActivity, menu: GameMenu, modifier: Modifier) {
    Column(modifier.padding(horizontal = 14.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        when (menu.page) {
            GameMenu.Page.Main -> SidebarMain(activity, menu)
            GameMenu.Page.Achievements -> SidebarAchievements(activity, menu)
        }
    }
}

@Composable private fun ColumnScope.SidebarMain(activity: RuntimeActivity, menu: GameMenu) {
    var controlsVisible by remember { mutableStateOf(activity.uiGameControlsVisible()) }
    var fpsVisible by remember { mutableStateOf(activity.uiFpsVisible()) }
    // The summary line is read from the files the runtime keeps, off the main thread.
    val summary by produceState<String?>(null) {
        value = withContext(Dispatchers.IO) {
            Achievements.load(activity.filesDir)?.let { "${it.unlocked.size} de ${it.count()} · ${it.earnedGamerscore} G" }
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("RENUT / JOGO", color = Accent, fontWeight = FontWeight.Bold, fontSize = 11.sp, letterSpacing = 1.sp)
            Text("Menu do jogo", color = Text, fontWeight = FontWeight.Bold, fontSize = 20.sp)
        }
        IconButton(onClick = { menu.close() }, modifier = Modifier.size(40.dp)) { Icon(Icons.Default.Close, "Fechar menu", tint = Muted) }
    }
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        SidebarItem(Icons.Default.PlayArrow, "Continuar", highlighted = true) { menu.close() }
        if (menu.isPaused) SidebarItem(Icons.Default.PlayArrow, "Retomar jogo", "O jogo está pausado") {
            menu.requestPaused(false); menu.close()
        } else SidebarItem(Icons.Default.Pause, "Pausar jogo", "Congela o jogo até você retomar") {
            menu.requestPaused(true); menu.close()
        }
        SidebarItem(Icons.Default.EmojiEvents, "Conquistas", summary ?: "Ver a lista do jogo", chevron = true) {
            menu.page = GameMenu.Page.Achievements
        }
        SidebarToggle(Icons.Default.SportsEsports, "Controles na tela", controlsVisible) {
            controlsVisible = it
            activity.uiSetGameControlsVisible(it)
        }
        SidebarToggle(Icons.Default.Speed, "Contador de FPS", fpsVisible) {
            fpsVisible = it
            activity.uiSetFpsVisible(it)
        }
        SidebarItem(Icons.AutoMirrored.Filled.ExitToApp, "Sair do jogo") { activity.finish() }
    }
}

@Composable private fun ColumnScope.SidebarAchievements(activity: RuntimeActivity, menu: GameMenu) {
    // The game keeps writing these while it runs; the stamp is cheap, the load only follows a change.
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(1000); tick++ } }
    @Suppress("UNUSED_VARIABLE") val update = tick
    val stamp = Achievements.stamp(activity.filesDir)
    val loaded = remember { mutableStateOf(false) }
    val snapshot by produceState<Achievements.Snapshot?>(null, stamp) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
        loaded.value = true
    }
    val data = snapshot
    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = { menu.page = GameMenu.Page.Main }) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, "Voltar ao menu", tint = Text)
        }
        Text("Conquistas", Modifier.weight(1f), color = Text, fontWeight = FontWeight.Bold, fontSize = 22.sp)
        IconButton(onClick = { menu.close() }) { Icon(Icons.Default.Close, "Fechar menu", tint = Muted) }
    }
    if (data == null) {
        if (loaded.value) Text(
            "A lista é lida do seu jogo na primeira vez em que ele inicia. Ela aparece aqui depois dessa primeira partida.",
            color = Muted, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 4.dp))
        return
    }
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Column(Modifier.padding(horizontal = 4.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row {
                    Text("${data.unlocked.size} de ${data.count()} desbloqueadas", Modifier.weight(1f), color = Text,
                        fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                    Text("${data.earnedGamerscore} de ${data.gamerscore} G", color = Accent, fontWeight = FontWeight.Bold, fontSize = 13.sp)
                }
                LinearProgressIndicator(
                    progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                    modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                    color = Accent, trackColor = Color(0xFF3A3B3D))
            }
        }
        if (data.unlocked.isNotEmpty()) {
            item { Text("Desbloqueadas", color = Accent, fontWeight = FontWeight.Bold, fontSize = 14.sp) }
            items(data.unlocked, key = { it.id }) { AchievementRow(it, compact = true) }
        }
        if (data.locked.isNotEmpty()) {
            item { Text("Bloqueadas", color = Accent, fontWeight = FontWeight.Bold, fontSize = 14.sp) }
            items(data.locked, key = { it.id }) { AchievementRow(it, compact = true) }
        }
    }
}

@Composable private fun SidebarItem(
    icon: ImageVector, title: String, subtitle: String? = null, highlighted: Boolean = false,
    chevron: Boolean = false, onClick: () -> Unit
) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp))
            .background(if (highlighted) Color(0xFF4B301F) else Color(0xFF26282B))
            .clickable(onClick = onClick).padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(icon, null, tint = if (highlighted) Accent else Text, modifier = Modifier.size(24.dp))
        Column(Modifier.weight(1f)) {
            Text(title, color = if (highlighted) Accent else Text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
            if (subtitle != null) Text(subtitle, color = Muted, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (chevron) Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, null, tint = Muted)
    }
}

@Composable private fun SidebarToggle(icon: ImageVector, title: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Color(0xFF26282B))
            .clickable { onCheckedChange(!checked) }.padding(start = 12.dp, end = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(icon, null, tint = Text, modifier = Modifier.size(24.dp))
        Text(title, Modifier.weight(1f), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        Switch(checked = checked, onCheckedChange = null, modifier = Modifier.scale(0.8f))
    }
}

@Composable private fun ControllerScreen(activity: ControllerSettingsActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    var chooseDevice by remember { mutableStateOf(false) }
    val devices = activity.uiDevices()
    Scaffold(containerColor = Background, topBar = {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { activity.finish() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Voltar") }
            Text("Controle externo", color = Text, fontWeight = FontWeight.Bold, fontSize = 21.sp)
        }
    }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            item { Text("Conecte um controle USB ou Bluetooth. O mapeamento é salvo para cada aparelho.", color = Muted) }
            item {
                Panel("Dispositivo") {
                    ActionButton(devices.getOrNull(activity.uiSelected()) ?: "Nenhum controle conectado", true, outlined = true) { chooseDevice = true }
                    ActionButton("Atualizar lista de controles", true, outlined = true) { activity.uiRefresh(); tick++ }
                }
            }
            item {
                Panel("Mapeamento") {
                    Text(activity.uiStatus(), color = Muted, fontSize = 14.sp)
                    ActionButton("Mapear automaticamente", devices.isNotEmpty()) { activity.uiAutomatic(); tick++ }
                    if (activity.uiPending() >= 0) ActionButton("Cancelar captura", true, outlined = true) { activity.uiCancel(); tick++ }
                }
            }
            items(activity.uiBindings().withIndex().toList()) { (index, label) ->
                Card(Modifier.fillMaxWidth().clickable { activity.uiCapture(index); tick++ },
                    colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(14.dp)) {
                    Text(label, Modifier.padding(16.dp), color = if (activity.uiPending() == index) Accent else Text)
                }
            }
        }
    }
    if (chooseDevice) AlertDialog(onDismissRequest = { chooseDevice = false },
        title = { Text("Selecionar controle") },
        text = { Column { devices.forEachIndexed { index, name ->
            Text(name, Modifier.fillMaxWidth().clickable { activity.uiSelect(index); chooseDevice = false; tick++ }.padding(12.dp))
        } } },
        confirmButton = { TextButton(onClick = { chooseDevice = false }) { Text("Fechar") } })
}

@Composable private fun ProbeScreen(activity: ProbeActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    Column(Modifier.fillMaxSize().background(Background).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Header("TESTE GRÁFICO", "Plume", "Apresentação Vulkan no aparelho")
        Panel("Resultado") { Text(activity.uiStatus(), color = Text, fontSize = 13.sp) }
        AndroidView(factory = { SurfaceView(activity).also(activity::attachComposeSurface) }, modifier = Modifier.fillMaxWidth().weight(1f))
        ActionButton("Voltar à biblioteca", true) { activity.finish() }
    }
}

@Composable private fun RuntimeScreen(activity: RuntimeActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    Column(Modifier.fillMaxSize().background(Background).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Header("TESTE DO RUNTIME", if (activity.uiGraphics()) "RexGlue + Plume" else "Runtime ARM64", "Validação experimental")
        Panel("Resultado") { Text(activity.uiStatus(), color = Text, fontSize = 13.sp) }
        if (activity.uiGraphics()) AndroidView(factory = { SurfaceView(activity).also(activity::attachComposeSurface) }, modifier = Modifier.fillMaxWidth().weight(1f))
        else Spacer(Modifier.weight(1f))
        ActionButton("Voltar", true) { activity.finish() }
    }
}
