package org.renut.android

import android.app.Activity
import android.graphics.BitmapFactory
import android.view.SurfaceView
import android.view.View
import androidx.activity.ComponentActivity
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Delete
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.activity.compose.BackHandler
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import android.widget.Toast
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.ExitToApp
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Gamepad
import androidx.compose.material.icons.filled.Language as LanguageIcon
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material.icons.filled.Vibration
import androidx.compose.material.icons.filled.Speed
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
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
import android.content.Context
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Thermostat
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.Canvas
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

/**
 * The move between two pages of one screen: the new page slides in a short way from the side it
 * is "ahead" in (forward = from the right) while the old one slides off and fades.
 */
private fun pageTransition(forward: Boolean): ContentTransform {
    val travel: (Int) -> Int = { width -> if (forward) width / 5 else -width / 5 }
    return (slideInHorizontally(tween(300)) { travel(it) } + fadeIn(tween(300))) togetherWith
        (slideOutHorizontally(tween(300)) { -travel(it) } + fadeOut(tween(160)))
}

private val Background = Color(0xFF100A06)
private val Surface = Color(0xFF1E140C)
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
    @JvmStatic fun showRuntime(activity: RuntimeActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { RuntimeScreen(activity) } } })
    }
    @JvmStatic fun createGameMenu(activity: RuntimeActivity): GameMenu = GameMenu(activity)
    @JvmStatic fun showLayoutList(activity: LayoutEditorActivity, model: LayoutListModel) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { LayoutListScreen(activity, model) } } })
    }
    @JvmStatic fun showLayoutEditor(activity: LayoutEditorActivity, model: LayoutEditorModel) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { LayoutEditorScreen(activity, model) } } })
    }
    @JvmStatic fun showWelcome(activity: WelcomeActivity, model: WelcomeModel) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { WelcomeScreen(activity, model) } } })
    }
}

/** What the first-run screens show; WelcomeActivity (Java) writes it, Compose observes it. */
class WelcomeModel {
    companion object {
        const val INTRO = 0
        const val NOTICE = 1
        const val INSTALL = 2
        const val DONE = 3
        const val FAILED = 4
    }
    var step by mutableIntStateOf(INTRO)
    var message by mutableStateOf("")
    /** 0..1 while installing; negative when the size is not known (indeterminate bar). */
    var fraction by mutableFloatStateOf(0f)
    var error by mutableStateOf("")
    /** True when the full ISO is being installed, false for the XEX alone. */
    var wholeDisc by mutableStateOf(true)
}

/**
 * The in-game sidebar. The view stays on screen for the whole game and only draws what is
 * open (a small tab, the sidebar, or the paused chip), so touches elsewhere reach the game.
 * RuntimeActivity (Java) drives it; Compose observes the state below.
 */
class GameMenu internal constructor(private val activity: RuntimeActivity) {
    internal enum class Page { Main, Achievements, Controllers }

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
    // Every stringResource below reads this context, so choosing another language re-renders in place.
    val host = LocalContext.current
    val live = LocalConfiguration.current  // read before it is replaced below: this one follows rotation
    val language = Language.code.ifEmpty { Language.OPTIONS[0].code }
    // The context only has to carry the locale for stringResource; it is not rebuilt on rotation.
    val localized = remember(host, language) { Language.derive(host, live, language) }
    // The configuration screens read (orientation, size) is copied fresh from the live one.
    val configuration = Language.configuration(live, language)
    CompositionLocalProvider(
        LocalContext provides localized,
        LocalConfiguration provides configuration
    ) { RenutMaterial(content) }
}

@Composable private fun RenutMaterial(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Accent, onPrimary = Background, background = Background,
            surface = Surface, onSurface = Text, onBackground = Text,
            secondary = Accent, secondaryContainer = Color(0xFF4B301F),
            onSecondaryContainer = Accent, surfaceVariant = Surface,
            onSurfaceVariant = Muted, outline = Color(0xFF3E2C1F)
        ), content = content
    )
}

private class NavEntry(val icon: ImageVector, val label: Int)

private val NAV_ENTRIES = listOf(
    NavEntry(Icons.Default.PlayArrow, R.string.nav_play),
    NavEntry(Icons.Default.EmojiEvents, R.string.nav_achievements),
    NavEntry(Icons.Default.Settings, R.string.nav_settings))

/**
 * The landscape navigation: one column on the left edge whose items share its whole height, so
 * each target is a large block (icon and label) rather than a small icon in the middle.
 */
@Composable private fun LauncherRail(selected: Int, onSelect: (Int) -> Unit) {
    Column(
        Modifier.fillMaxHeight().width(108.dp).background(Surface)
            .windowInsetsPadding(WindowInsets.systemBars.only(WindowInsetsSides.Vertical + WindowInsetsSides.Start))
            .padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        NAV_ENTRIES.forEachIndexed { index, entry ->
            val active = index == selected
            Column(
                Modifier.weight(1f).fillMaxWidth().clip(RoundedCornerShape(20.dp))
                    .background(if (active) Color(0xFF4B301F) else Color.Transparent)
                    .clickable { onSelect(index) },
                verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Icon(entry.icon, null, tint = if (active) Accent else Muted, modifier = Modifier.size(36.dp))
                Spacer(Modifier.height(6.dp))
                Text(
                    stringResource(entry.label), color = if (active) Text else Muted, fontSize = 12.sp,
                    fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal, maxLines = 1, softWrap = false)
            }
        }
    }
}

@Composable private fun MainScreen(activity: MainActivity) {
    // Saved, so the page in focus survives a rotation or the system recreating the screen.
    var page by rememberSaveable { mutableIntStateOf(0) }
    // The photo album opens over the Play page and closes when another page is chosen.
    var photosOpen by rememberSaveable { mutableStateOf(false) }
    // Likewise the saves, reached from a card on the Play page.
    var savesOpen by rememberSaveable { mutableStateOf(false) }
    var tick by remember { mutableIntStateOf(0) }
    BackHandler(photosOpen || savesOpen) { photosOpen = false; savesOpen = false }
    LaunchedEffect(Unit) { while (true) { delay(700); tick++ } }

    @Composable fun Pages(modifier: Modifier) {
        if (photosOpen) {
            PhotosScreen(activity, modifier, tick) { photosOpen = false }
            return
        }
        if (savesOpen) {
            SaveScreen(activity, modifier, tick) { savesOpen = false }
            return
        }
        AnimatedContent(
            targetState = page, label = "launcher-page", modifier = modifier,
            transitionSpec = { pageTransition(targetState > initialState) }
        ) { shown ->
            when (shown) {
                0 -> PlayScreen(activity, Modifier, tick, { page = 1 }, { photosOpen = true }) { savesOpen = true }
                1 -> AchievementsScreen(activity, Modifier, tick)
                else -> SettingsScreen(activity, Modifier, tick)
            }
        }
    }

    // Decided by the size the screen really has, not by a configuration value: this activity is not
    // recreated on rotation, and the size is what the layout has to fit anyway.
    BoxWithConstraints(Modifier.fillMaxSize().background(Background)) {
    val landscape = maxWidth > maxHeight
    if (landscape) {
        // Wide and short: the icons stand in a column on the left, the page takes the rest.
        Row(Modifier.fillMaxSize().background(Background)) {
            LauncherRail(page) { photosOpen = false; savesOpen = false; page = it }
            // A page does not need the whole width of a landscape screen.
            // Scaffold keeps the page clear of the bars in portrait; here it is done by hand.
            Box(
                Modifier.weight(1f).fillMaxHeight()
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical + WindowInsetsSides.End)),
                contentAlignment = Alignment.TopCenter
            ) {
                Pages(Modifier.widthIn(max = 840.dp).fillMaxSize())
            }
        }
    } else {
        Scaffold(
            containerColor = Background,
            bottomBar = {
                NavigationBar(containerColor = Surface) {
                    NAV_ENTRIES.forEachIndexed { index, entry ->
                        NavigationBarItem(selected = page == index, onClick = { photosOpen = false; savesOpen = false; page = index },
                            icon = { Icon(entry.icon, null) }, label = { Text(stringResource(entry.label)) })
                    }
                }
            }
        ) { padding -> Pages(Modifier.padding(padding).fillMaxSize()) }
    }
    }
}

/** The saved games: what the game keeps on this device, with export and import as a zip. */
@Composable private fun SaveScreen(activity: MainActivity, modifier: Modifier, tick: Int, onBack: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val stamp = PhotoAlbum.stamp(activity.filesDir)
    // Listing looks inside the packages (to tell photos from saves), so it runs off the main thread.
    val saves by produceState(emptyList<String>(), stamp) {
        value = withContext(Dispatchers.IO) { runCatching { activity.uiSaves() }.getOrDefault(emptyList()) }
    }
    // The Headers and each package's __thumbnail.png are the game's own bookkeeping, not saves of their own.
    val visibleSaves = saves.filterNot { it.contains("/Headers/") || it.endsWith("/__thumbnail.png") }.ifEmpty { saves }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { BackLink(onBack) }
        item { Header(stringResource(R.string.saves_eyebrow), stringResource(R.string.saves_title), stringResource(R.string.saves_subtitle)) }
        item {
            Panel(stringResource(R.string.saves_panel_library)) {
                Text(if (visibleSaves.isEmpty()) stringResource(R.string.saves_none) else stringResource(R.string.saves_found, visibleSaves.size), color = Text, fontWeight = FontWeight.SemiBold)
                Text(stringResource(R.string.saves_note), color = Muted, fontSize = 13.sp)
            }
        }
        if (visibleSaves.isNotEmpty()) {
            item { SectionTitle(stringResource(R.string.saves_section_files)) }
            items(visibleSaves) { name -> Panel(null) {
                Text(name.substringAfterLast('/'), color = Text, fontWeight = FontWeight.SemiBold)
                Text(stringResource(R.string.saves_item), color = Muted, fontSize = 13.sp)
            } }
        }
        item {
            Panel(stringResource(R.string.saves_panel_transfer)) {
                ActionButton(stringResource(R.string.saves_export), !activity.uiBusy()) { activity.uiExportSaves() }
                ActionButton(stringResource(R.string.saves_import), !activity.uiBusy(), outlined = true) { activity.uiImportSaves() }
                Text(stringResource(R.string.saves_import_note), color = Muted, fontSize = 12.sp)
            }
        }
    }
}

@Composable private fun BackLink(onBack: () -> Unit) {
    Row(
        Modifier.clip(RoundedCornerShape(12.dp)).clickable { onBack() }.padding(end = 12.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(Icons.AutoMirrored.Filled.ArrowBack, null, tint = Accent)
        Spacer(Modifier.width(6.dp))
        Text(stringResource(R.string.common_back), color = Accent, fontWeight = FontWeight.SemiBold)
    }
}

/** The entry to the saves, on the Play page. */
@Composable private fun SavesCard(open: () -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp),
        modifier = Modifier.fillMaxWidth().clickable { open() }) {
        Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.Folder, null, tint = Accent, modifier = Modifier.size(32.dp))
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.play_saves_card), color = Text, fontWeight = FontWeight.SemiBold)
                Text(stringResource(R.string.saves_subtitle), color = Muted, fontSize = 13.sp)
            }
            Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, null, tint = Muted)
        }
    }
}

@Composable private fun PlayScreen(activity: MainActivity, modifier: Modifier, tick: Int, openAchievements: () -> Unit, openPhotos: () -> Unit, openSaves: () -> Unit) {
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
        item { Header(stringResource(R.string.play_eyebrow), stringResource(R.string.play_title), stringResource(R.string.play_subtitle)) }
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
                                2 -> stringResource(R.string.play_state_full)
                                1 -> stringResource(R.string.play_state_xex)
                                else -> stringResource(R.string.play_state_none)
                            }, color = Muted, fontSize = 13.sp)
                        }
                    }
                    Button(
                        onClick = { activity.uiStartGame() }, enabled = state == 2 && !busy,
                        modifier = Modifier.fillMaxWidth().height(60.dp), shape = RoundedCornerShape(16.dp)
                    ) {
                        Icon(Icons.Default.PlayArrow, null, modifier = Modifier.size(28.dp))
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.nav_play), fontWeight = FontWeight.Bold, fontSize = 19.sp)
                    }
                    if (state != 2) Text(
                        if (state == 1) stringResource(R.string.play_hint_xex)
                        else stringResource(R.string.play_hint_none),
                        color = Muted, fontSize = 13.sp)
                    else Text(stringResource(R.string.play_experimental), color = Muted, fontSize = 12.sp)
                    if (state == 2 && activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true")
                        Text(stringResource(R.string.play_cheats_on), color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
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
                            Text(stringResource(R.string.play_ach_progress, data.unlocked.size, data.count()), color = Text, fontWeight = FontWeight.SemiBold)
                            LinearProgressIndicator(
                                progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                                modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                                color = Accent, trackColor = Color(0xFF3E2C1F))
                        }
                        Text("${data.earnedGamerscore} G", color = Accent, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
        item { PhotosCard(activity, openPhotos) }
        item { SavesCard(openSaves) }
        item { CheatsPanel(activity, !busy) }
        item {
            Panel(stringResource(R.string.play_panel_open)) {
                ToggleRow(stringResource(R.string.play_autostart), autoStart, !busy) { activity.uiSetAutoStart(it) }
                Text(stringResource(R.string.play_autostart_note), color = Muted, fontSize = 12.sp)
            }
        }
    }
}

/** The entry to the photo album, on the Play page: how many photos the game has saved. */
@Composable private fun PhotosCard(activity: MainActivity, open: () -> Unit) {
    val stamp = PhotoAlbum.stamp(activity.filesDir)
    val count by produceState(-1, stamp) {
        value = withContext(Dispatchers.IO) { PhotoAlbum.load(activity.filesDir).size }
    }
    Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp),
        modifier = Modifier.fillMaxWidth().clickable { open() }) {
        Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.PhotoLibrary, null, tint = Accent, modifier = Modifier.size(32.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(R.string.play_photos_card), color = Text, fontWeight = FontWeight.SemiBold)
                Text(
                    if (count > 0) stringResource(R.string.play_photos_count, count) else stringResource(R.string.play_photos_none),
                    color = Muted, fontSize = 13.sp)
            }
            Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, null, tint = Muted)
        }
    }
}

/** The pictures of the game's Photo Mode: a grid, and a full-screen viewer to swipe through. */
@Composable private fun PhotosScreen(activity: MainActivity, modifier: Modifier, tick: Int, onBack: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val stamp = PhotoAlbum.stamp(activity.filesDir)
    val loaded = remember { mutableStateOf(false) }
    val photos by produceState(emptyList<PhotoAlbum.Photo>(), stamp) {
        value = withContext(Dispatchers.IO) { PhotoAlbum.load(activity.filesDir) }
        loaded.value = true
    }
    var viewing by rememberSaveable { mutableIntStateOf(-1) }
    LazyVerticalGrid(
        GridCells.Adaptive(170.dp), modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item(span = { GridItemSpan(maxLineSpan) }) {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                BackLink(onBack)
                Header(stringResource(R.string.photos_eyebrow), stringResource(R.string.photos_title), stringResource(R.string.photos_subtitle))
                if (photos.isNotEmpty())
                    ActionButton(stringResource(R.string.photos_export), !activity.uiBusy(), outlined = true) { activity.uiExportPhotos() }
            }
        }
        if (photos.isEmpty() && loaded.value) {
            item(span = { GridItemSpan(maxLineSpan) }) {
                Panel(null) { Text(stringResource(R.string.photos_empty), color = Muted, fontSize = 14.sp) }
            }
        }
        itemsIndexed(photos, key = { _, photo -> photo.id() }) { index, photo ->
            val thumb by produceState<ImageBitmap?>(null, photo.id()) {
                value = withContext(Dispatchers.IO) { PhotoAlbum.decode(photo, 480)?.asImageBitmap() }
            }
            Box(
                Modifier.fillMaxWidth().aspectRatio(16f / 9f).clip(RoundedCornerShape(14.dp))
                    .background(Color(0xFF38271A)).clickable { viewing = index }
            ) {
                thumb?.let { Image(it, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop) }
            }
        }
    }
    if (viewing in photos.indices) PhotoViewer(photos, viewing) { viewing = -1 }
    else if (viewing >= 0 && loaded.value) viewing = -1
}

@Composable private fun PhotoViewer(photos: List<PhotoAlbum.Photo>, start: Int, onClose: () -> Unit) {
    val pager = rememberPagerState(initialPage = start) { photos.size }
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val untitled = stringResource(R.string.photos_untitled)
    val saved = stringResource(R.string.photos_saved)
    val failed = stringResource(R.string.photos_save_failed)
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            HorizontalPager(pager, Modifier.fillMaxSize(), key = { photos[it].id() }) { page ->
                val photo = photos[page]
                val bitmap by produceState<ImageBitmap?>(null, photo.id()) {
                    value = withContext(Dispatchers.IO) { PhotoAlbum.decode(photo, 2048)?.asImageBitmap() }
                }
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    bitmap?.let { Image(it, null, Modifier.fillMaxSize(), contentScale = ContentScale.Fit) }
                }
            }
            val photo = photos[pager.currentPage.coerceIn(photos.indices)]
            Row(
                Modifier.align(Alignment.TopCenter).fillMaxWidth().background(Color(0x99000000))
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
                    .padding(horizontal = 8.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = onClose) { Icon(Icons.Default.Close, stringResource(R.string.common_close), tint = Color.White) }
                Column(Modifier.weight(1f)) {
                    Text(photo.title.ifBlank { untitled }, color = Color.White, fontWeight = FontWeight.SemiBold, maxLines = 1,
                        overflow = TextOverflow.Ellipsis)
                    Text(DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT, Language.locale(context))
                        .format(Date(photo.takenAt)), color = Color(0xFFCCCCCC), fontSize = 12.sp)
                }
                Text(stringResource(R.string.photos_counter, pager.currentPage + 1, photos.size), color = Color(0xFFCCCCCC), fontSize = 13.sp)
                Spacer(Modifier.width(8.dp))
                Button(onClick = {
                    scope.launch {
                        val ok = withContext(Dispatchers.IO) { PhotoAlbum.saveToGallery(context.contentResolver, photo) }
                        Toast.makeText(context, if (ok) saved else failed, Toast.LENGTH_SHORT).show()
                    }
                }) { Text(stringResource(R.string.photos_save)) }
            }
        }
    }
}

private val CHEAT_LABELS = listOf(
    "renut_cheat_infinite_health" to R.string.cheat_infinite_health,
    "renut_cheat_infinite_fuel_ammo" to R.string.cheat_infinite_fuel_ammo,
    "renut_cheat_infinite_parts" to R.string.cheat_infinite_parts,
    "renut_cheat_no_timer" to R.string.cheat_no_timer,
    "renut_cheat_no_notes_spent" to R.string.cheat_no_notes_spent,
    "renut_cheat_vehicles_anywhere" to R.string.cheat_vehicles_anywhere,
    "renut_cheat_extended_build_range" to R.string.cheat_extended_build_range,
    "renut_cheat_super_punch" to R.string.cheat_super_punch)
private val SKIN_LABELS = listOf(
    "default" to R.string.skin_default, "robot" to R.string.skin_robot, "tuxedo" to R.string.skin_tuxedo)

/** Cheats of the next game start. The switches live in renut.toml, which the runtime reads at launch. */
@Composable private fun CheatsPanel(activity: MainActivity, enabled: Boolean) {
    var master by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true") }
    val flags = remember {
        mutableStateMapOf<String, Boolean>().apply {
            CHEAT_LABELS.forEach { (key, _) -> put(key, activity.uiRuntimeOption(key, "false") == "true") }
        }
    }
    var skin by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEAT_SKIN, "default")) }
    Panel(stringResource(R.string.cheats_panel)) {
        ToggleRow(stringResource(R.string.cheats_master), master, enabled) {
            master = it
            activity.uiSetRuntimeOption(RuntimeOptions.CHEATS, it.toString())
        }
        if (master) {
            CHEAT_LABELS.forEach { (key, label) ->
                ToggleRow(stringResource(label), flags[key] == true, enabled) {
                    flags[key] = it
                    activity.uiSetRuntimeOption(key, it.toString())
                }
            }
            val skinNames = SKIN_LABELS.map { stringResource(it.second) }
            SingleChoiceSelector(stringResource(R.string.cheats_skin),
                skinNames[SKIN_LABELS.indexOfFirst { it.first == skin }.coerceAtLeast(0)], skinNames) { label ->
                val value = SKIN_LABELS[skinNames.indexOf(label).coerceAtLeast(0)].first
                skin = value
                activity.uiSetRuntimeOption(RuntimeOptions.CHEAT_SKIN, value)
            }
        }
        Text(stringResource(R.string.cheats_note), color = Muted, fontSize = 12.sp)
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
        item { Header(stringResource(R.string.ach_eyebrow), stringResource(R.string.ach_title), stringResource(R.string.ach_subtitle)) }
        if (data == null) {
            if (loaded.value) item {
                Panel(stringResource(R.string.ach_panel_nolist)) {
                    Text(stringResource(R.string.ach_nolist), color = Muted, fontSize = 14.sp)
                }
            }
        } else {
            item {
                Panel(stringResource(R.string.ach_panel_progress)) {
                    Text(stringResource(R.string.ach_unlocked_count, data.unlocked.size, data.count()), color = Text, fontWeight = FontWeight.SemiBold)
                    LinearProgressIndicator(
                        progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                        modifier = Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                        color = Accent, trackColor = Color(0xFF3E2C1F))
                    Text(stringResource(R.string.ach_gamerscore, data.earnedGamerscore, data.gamerscore), color = Muted, fontSize = 13.sp)
                }
            }
            if (data.unlocked.isNotEmpty()) {
                item { SectionTitle(stringResource(R.string.ach_section_unlocked)) }
                items(data.unlocked, key = { it.id }) { AchievementRow(it) }
            }
            if (data.locked.isNotEmpty()) {
                item { SectionTitle(stringResource(R.string.ach_section_locked)) }
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
    Card(colors = CardDefaults.cardColors(containerColor = if (compact) Color(0xFF2A1C11) else Surface),
        shape = RoundedCornerShape(if (compact) 12.dp else 16.dp), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(pad), horizontalArrangement = Arrangement.spacedBy(pad), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(if (compact) 42.dp else 56.dp).clip(RoundedCornerShape(if (compact) 9.dp else 12.dp))
                .background(Color(0xFF38271A)).alpha(if (unlocked) 1f else 0.45f)) {
                if (icon != null) Image(icon, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop, colorFilter = if (unlocked) null else gray)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(item.label, color = if (unlocked) Text else Muted, fontWeight = FontWeight.Bold, fontSize = if (compact) 14.sp else 16.sp)
                Text(item.shownDescription(), color = Muted, fontSize = if (compact) 12.sp else 13.sp)
                if (unlocked) Text(
                    stringResource(R.string.ach_unlocked_on, DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT,
                        Language.locale(LocalContext.current)).format(Date(item.unlockedAt))),
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
        title = { Text(stringResource(R.string.driver_delete_title)) },
        text = { Text(stringResource(R.string.driver_delete_message, driver)) },
        confirmButton = { TextButton(onClick = { confirmDeleteDriver = false; activity.uiDeleteDriver() }) { Text(stringResource(R.string.common_delete)) } },
        dismissButton = { TextButton(onClick = { confirmDeleteDriver = false }) { Text(stringResource(R.string.common_cancel)) } })
    val busy = activity.uiBusy()
    val fpsEnabled = activity.uiFpsEnabled()
    val tempEnabled = activity.uiTempEnabled()
    val controlsEnabled = activity.uiShowControls()
    val oneUiEnabled = activity.uiOneUiEnabled()
    val tuMode = activity.uiTuMode()
    val ir3Mode = activity.uiIr3Mode()
    val turnip = remember(driver) { activity.uiDriverIsTurnip() }
    val logLevel = activity.uiRuntimeOption("log_level", "info")
    val noShadows = activity.uiRuntimeOption("renut_plume_no_shadows", "false") == "true"
    val sustained = activity.uiRuntimeOption("renut_adpf", "true") == "true"
    val textureLevels = listOf(stringResource(R.string.texture_quality_full), stringResource(R.string.texture_quality_half),
        stringResource(R.string.texture_quality_quarter))
    var textureQuality by remember {
        mutableStateOf((activity.uiRuntimeOption(RuntimeOptions.TEXTURE_QUALITY, "0").toIntOrNull() ?: 0).coerceIn(0, 2))
    }
    val shadowLevels = listOf(stringResource(R.string.shadow_quality_full), stringResource(R.string.shadow_quality_medium),
        stringResource(R.string.shadow_quality_low))
    var shadowQuality by remember {
        mutableStateOf((activity.uiRuntimeOption(RuntimeOptions.SHADOW_QUALITY, "0").toIntOrNull() ?: 0).coerceIn(0, 2))
    }
    val resolutionLevels = listOf(stringResource(R.string.internal_resolution_full),
        stringResource(R.string.internal_resolution_three_quarters), stringResource(R.string.internal_resolution_half))
    var internalResolution by remember {
        mutableStateOf((activity.uiRuntimeOption(RuntimeOptions.INTERNAL_RESOLUTION, "0").toIntOrNull() ?: 0).coerceIn(0, 2))
    }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item {
            Row(verticalAlignment = Alignment.Top) {
                Box(Modifier.weight(1f)) {
                    Header(stringResource(R.string.settings_eyebrow), stringResource(R.string.settings_title), stringResource(R.string.settings_subtitle))
                }
                LanguageMenu()
            }
        }
        item {
            Panel(stringResource(R.string.settings_game)) {
                Text(when (activity.uiGameState()) {
                    2 -> stringResource(R.string.settings_game_full)
                    1 -> stringResource(R.string.settings_game_xex)
                    else -> stringResource(R.string.settings_game_none)
                }, color = Muted, fontSize = 13.sp)
                ActionButton(stringResource(R.string.settings_import_game), !busy, outlined = true) { activity.uiOpenGameImport() }
                Text(stringResource(R.string.settings_import_note), color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel(stringResource(R.string.settings_display)) {
                ToggleRow(stringResource(R.string.settings_fps), fpsEnabled, !busy) { activity.uiSetFps(it) }
                ToggleRow(stringResource(R.string.settings_temp), tempEnabled, !busy) { activity.uiSetTemp(it) }
                FpsTargetSlider(RuntimeOptions.fpsTarget(activity), !busy, onLive = {}) {
                    activity.uiSetRuntimeOption(RuntimeOptions.FPS_TARGET, it.toString())
                }
                Text(stringResource(R.string.settings_fps_target_note), color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel(stringResource(R.string.settings_controls)) {
                ToggleRow(stringResource(R.string.settings_show_controls), controlsEnabled, !busy) { activity.uiSetShowControls(it) }
                ActionButton(stringResource(R.string.layouts_manage), !busy, outlined = true) { activity.uiOpenLayoutEditor() }
                ActionButton(stringResource(R.string.settings_external), !busy, outlined = true) { activity.uiOpenControllerSettings() }
            }
        }
        item {
            Panel(stringResource(R.string.settings_advanced)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    SingleChoiceSelector(stringResource(R.string.settings_driver), driver, activity.uiDriverChoices(), !busy, Modifier.weight(1f)) { activity.uiSelectDriver(it) }
                    // Only a driver the user imported can be removed; the bundled and system ones stay.
                    if (activity.uiDriverStored()) IconButton(onClick = { confirmDeleteDriver = true }, enabled = !busy) {
                        Icon(Icons.Default.Delete, stringResource(R.string.settings_driver_delete), tint = if (busy) Muted else Accent)
                    }
                }
                ActionButton(stringResource(R.string.settings_driver_import), !busy, outlined = true) { activity.uiPickDriver() }
                if (busy) Text(activity.uiStatus(), color = Muted, fontSize = 13.sp, maxLines = 3, overflow = TextOverflow.Ellipsis)
                if (turnip) {
                    ToggleRow("One UI fix", oneUiEnabled, !busy) { activity.uiSetOneUi(it) }
                    MultiModeSelector("TU_DEBUG", tuMode, listOf("perf", "sysmem", "gmem")) { activity.uiSetTuMode(it) }
                    MultiModeSelector("IR3_SHADER_DEBUG", ir3Mode, listOf("nopreamble", "noearlypreamble", "nouboopt", "nodescprefetch")) { activity.uiSetIr3Mode(it) }
                } else {
                    Text(stringResource(R.string.settings_turnip_note), color = Muted, fontSize = 12.sp)
                }
                ActionButton(stringResource(R.string.settings_clear_shaders), !busy, outlined = true) { activity.uiConfirmClearShaderCache() }
            }
        }
        item {
            Panel(stringResource(R.string.settings_runtime)) {
                Text(stringResource(R.string.settings_runtime_note), color = Muted, fontSize = 13.sp)
                SingleChoiceSelector(stringResource(R.string.settings_log_level), logLevel,
                    listOf("trace", "debug", "info", "warn", "error", "critical", "off")) {
                    activity.uiSetRuntimeOption("log_level", it)
                }
                SingleChoiceSelector(stringResource(R.string.settings_texture_quality), textureLevels[textureQuality],
                    textureLevels, !busy) { choice ->
                    val level = textureLevels.indexOf(choice).coerceAtLeast(0)
                    textureQuality = level
                    activity.uiSetRuntimeOption(RuntimeOptions.TEXTURE_QUALITY, level.toString())
                }
                SingleChoiceSelector(stringResource(R.string.settings_internal_resolution), resolutionLevels[internalResolution],
                    resolutionLevels, !busy) { choice ->
                    val level = resolutionLevels.indexOf(choice).coerceAtLeast(0)
                    internalResolution = level
                    activity.uiSetRuntimeOption(RuntimeOptions.INTERNAL_RESOLUTION, level.toString())
                }
                SingleChoiceSelector(stringResource(R.string.settings_shadow_quality), shadowLevels[shadowQuality],
                    shadowLevels, !busy) { choice ->
                    val level = shadowLevels.indexOf(choice).coerceAtLeast(0)
                    shadowQuality = level
                    activity.uiSetRuntimeOption(RuntimeOptions.SHADOW_QUALITY, level.toString())
                }
                ToggleRow(stringResource(R.string.settings_no_shadows), noShadows, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_no_shadows", it.toString())
                }
                ToggleRow(stringResource(R.string.settings_adpf), sustained, !busy) {
                    activity.uiSetRuntimeOption("renut_adpf", it.toString())
                }
            }
        }
        item { Text(stringResource(R.string.settings_footer), color = Muted, fontSize = 12.sp) }
    }
}

/** The language button: shows the current language and lists the others, each in its own language. */
@Composable private fun LanguageMenu(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var open by remember { mutableStateOf(false) }
    val current = Language.OPTIONS.firstOrNull { it.code == Language.code } ?: Language.OPTIONS[0]
    Box(modifier) {
        TextButton(onClick = { open = true }, contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp)) {
            Icon(Icons.Default.LanguageIcon, stringResource(R.string.language_label), tint = Accent, modifier = Modifier.size(20.dp))
            Spacer(Modifier.width(6.dp))
            Text(current.name, color = Text, fontSize = 14.sp)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            Language.OPTIONS.forEach { option ->
                DropdownMenuItem(
                    text = { Text(option.name) },
                    leadingIcon = { RadioButton(selected = option.code == current.code, onClick = null) },
                    onClick = { open = false; Language.set(context, option.code) })
            }
        }
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

@Composable private fun fpsLabel(fps: Int): String = when (fps) {
    RuntimeOptions.FPS_UNLIMITED -> stringResource(R.string.settings_fps_unlimited)
    RuntimeOptions.FPS_ORIGINAL -> stringResource(R.string.settings_fps_original, fps)
    else -> stringResource(R.string.settings_fps_value, fps)
}

/**
 * The frame-rate bar: 30 (the game's own) up to 240 in steps of 5, and "unlimited" at its end.
 * [onLive] runs on every step while the thumb moves; [onCommit] once, when it lets go.
 */
@Composable private fun FpsTargetSlider(initial: Int, enabled: Boolean, onLive: (Int) -> Unit, onCommit: (Int) -> Unit) {
    var position by remember { mutableFloatStateOf(RuntimeOptions.sliderFromFps(initial).toFloat()) }
    val fps = RuntimeOptions.fpsFromSlider(Math.round(position))
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.settings_fps_target), color = Text, modifier = Modifier.weight(1f))
            Text(fpsLabel(fps), color = Accent, fontWeight = FontWeight.Bold)
        }
        Slider(
            value = position,
            onValueChange = { position = it; onLive(RuntimeOptions.fpsFromSlider(Math.round(it))) },
            enabled = enabled,
            valueRange = 0f..RuntimeOptions.SLIDER_UNLIMITED.toFloat(),
            steps = RuntimeOptions.SLIDER_UNLIMITED - 1,
            onValueChangeFinished = { onCommit(RuntimeOptions.fpsFromSlider(Math.round(position))) })
    }
}

/** A dropdown's closed face: small label over the current value, with a caret, in a recessed field. */
@Composable private fun DropdownField(label: String, value: String, enabled: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(Color(0xFF120B06))
            .border(1.dp, Color(0xFF40301F), RoundedCornerShape(10.dp))
            .alpha(if (enabled) 1f else 0.5f)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(start = 14.dp, end = 8.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, color = Muted, fontSize = 12.sp)
            Text(value, color = Text, fontSize = 15.sp)
        }
        Icon(Icons.Default.ArrowDropDown, null, tint = Muted, modifier = Modifier.size(28.dp))
    }
}

@Composable private fun MultiModeSelector(label: String, selected: String, choices: List<String>, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    var local by remember { mutableStateOf(selected) }
    Box {
        DropdownField(label, (if (expanded) local else selected).ifEmpty { stringResource(R.string.common_disabled) }.replace(",", ", "), true) {
            local = selected; expanded = true
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            DropdownMenuItem(text = { Text(stringResource(R.string.common_disabled)) }, leadingIcon = {
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

@Composable private fun SingleChoiceSelector(label: String, selected: String, choices: List<String>, enabled: Boolean = true, modifier: Modifier = Modifier, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Box(modifier) {
        DropdownField(label, selected, enabled) { expanded = true }
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
                .background(Color(0xF21E140C))
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
    ) { Icon(Icons.Default.Menu, stringResource(R.string.menu_open), tint = Color(0xB3FFFFFF), modifier = Modifier.size(18.dp)) }
}

@Composable private fun PausedChip(menu: GameMenu, modifier: Modifier) {
    Row(
        modifier.clip(RoundedCornerShape(bottomStart = 18.dp, bottomEnd = 18.dp)).background(Color(0xE61E140C))
            .padding(horizontal = 14.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(Icons.Default.Pause, null, tint = Accent, modifier = Modifier.size(18.dp))
        Text(stringResource(R.string.menu_paused), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
        Button(onClick = { menu.requestPaused(false) }, contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
            modifier = Modifier.height(32.dp), shape = RoundedCornerShape(10.dp)) {
            Icon(Icons.Default.PlayArrow, null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(4.dp))
            Text(stringResource(R.string.menu_resume), fontSize = 13.sp)
        }
        IconButton(onClick = { menu.open() }, modifier = Modifier.size(32.dp)) {
            Icon(Icons.Default.Menu, stringResource(R.string.menu_open), tint = Muted, modifier = Modifier.size(20.dp))
        }
    }
}

@Composable private fun Sidebar(activity: RuntimeActivity, menu: GameMenu, modifier: Modifier) {
    Box(modifier.padding(horizontal = 14.dp, vertical = 8.dp)) {
        AnimatedContent(
            targetState = menu.page, label = "sidebar-page", modifier = Modifier.fillMaxSize(),
            transitionSpec = { pageTransition(targetState.ordinal > initialState.ordinal) }
        ) { shown ->
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                when (shown) {
                    GameMenu.Page.Main -> SidebarMain(activity, menu)
                    GameMenu.Page.Achievements -> SidebarAchievements(activity, menu)
                    GameMenu.Page.Controllers -> SidebarControllers(activity, menu)
                }
            }
        }
    }
}

@Composable private fun ColumnScope.SidebarMain(activity: RuntimeActivity, menu: GameMenu) {
    var controlsVisible by remember { mutableStateOf(activity.uiGameControlsVisible()) }
    var fpsVisible by remember { mutableStateOf(activity.uiFpsVisible()) }
    var tempVisible by remember { mutableStateOf(activity.uiTempVisible()) }
    var rumbleOn by remember { mutableStateOf(activity.uiRumbleEnabled()) }
    // The summary line is read from the files the runtime keeps, off the main thread.
    val snapshot by produceState<Achievements.Snapshot?>(null) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
    }
    val summary = snapshot?.let { stringResource(R.string.menu_ach_summary, it.unlocked.size, it.count(), it.earnedGamerscore) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(stringResource(R.string.menu_eyebrow), color = Accent, fontWeight = FontWeight.Bold, fontSize = 11.sp, letterSpacing = 1.sp)
            Text(stringResource(R.string.menu_title), color = Text, fontWeight = FontWeight.Bold, fontSize = 20.sp)
        }
        IconButton(onClick = { menu.close() }, modifier = Modifier.size(40.dp)) { Icon(Icons.Default.Close, stringResource(R.string.menu_close), tint = Muted) }
    }
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        SidebarItem(Icons.Default.PlayArrow, stringResource(R.string.menu_continue), highlighted = true) { menu.close() }
        if (menu.isPaused) SidebarItem(Icons.Default.PlayArrow, stringResource(R.string.menu_resume_game), stringResource(R.string.menu_paused_hint)) {
            menu.requestPaused(false); menu.close()
        } else SidebarItem(Icons.Default.Pause, stringResource(R.string.menu_pause_game), stringResource(R.string.menu_pause_hint)) {
            menu.requestPaused(true); menu.close()
        }
        SidebarItem(Icons.Default.EmojiEvents, stringResource(R.string.nav_achievements), summary ?: stringResource(R.string.menu_ach_hint), chevron = true) {
            menu.page = GameMenu.Page.Achievements
        }
        SidebarItem(Icons.Default.Gamepad, stringResource(R.string.menu_controllers), padSummary(activity), chevron = true) {
            menu.page = GameMenu.Page.Controllers
        }
        SidebarToggle(Icons.Default.Vibration, stringResource(R.string.ctrl_rumble), rumbleOn) {
            rumbleOn = it
            activity.uiSetRumble(it)
        }
        SidebarToggle(Icons.Default.SportsEsports, stringResource(R.string.menu_onscreen_controls), controlsVisible) {
            controlsVisible = it
            activity.uiSetGameControlsVisible(it)
        }
        SidebarToggle(Icons.Default.Speed, stringResource(R.string.settings_fps), fpsVisible) {
            fpsVisible = it
            activity.uiSetFpsVisible(it)
        }
        SidebarFpsTarget(activity)
        SidebarToggle(Icons.Default.Thermostat, stringResource(R.string.settings_temp), tempVisible) {
            tempVisible = it
            activity.uiSetTempVisible(it)
        }
        SidebarItem(Icons.Default.Tune, stringResource(R.string.layouts_manage)) {
            menu.close()
            activity.uiOpenLayoutEditor()
        }
        SidebarItem(Icons.AutoMirrored.Filled.ExitToApp, stringResource(R.string.menu_exit)) { activity.finish() }
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
            Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.menu_back), tint = Text)
        }
        Text(stringResource(R.string.nav_achievements), Modifier.weight(1f), color = Text, fontWeight = FontWeight.Bold, fontSize = 22.sp)
        IconButton(onClick = { menu.close() }) { Icon(Icons.Default.Close, stringResource(R.string.menu_close), tint = Muted) }
    }
    if (data == null) {
        if (loaded.value) Text(
            stringResource(R.string.menu_ach_none),
            color = Muted, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 4.dp))
        return
    }
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Column(Modifier.padding(horizontal = 4.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row {
                    Text(stringResource(R.string.ach_unlocked_count, data.unlocked.size, data.count()), Modifier.weight(1f), color = Text,
                        fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                    Text(stringResource(R.string.ach_gamerscore, data.earnedGamerscore, data.gamerscore), color = Accent, fontWeight = FontWeight.Bold, fontSize = 13.sp)
                }
                LinearProgressIndicator(
                    progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                    modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                    color = Accent, trackColor = Color(0xFF3E2C1F))
            }
        }
        if (data.unlocked.isNotEmpty()) {
            item { Text(stringResource(R.string.ach_section_unlocked), color = Accent, fontWeight = FontWeight.Bold, fontSize = 14.sp) }
            items(data.unlocked, key = { it.id }) { AchievementRow(it, compact = true) }
        }
        if (data.locked.isNotEmpty()) {
            item { Text(stringResource(R.string.ach_section_locked), color = Accent, fontWeight = FontWeight.Bold, fontSize = 14.sp) }
            items(data.locked, key = { it.id }) { AchievementRow(it, compact = true) }
        }
    }
}

/** The sidebar's page of connected controllers, one row each with the player it plays as. */
@Composable private fun ColumnScope.SidebarControllers(activity: RuntimeActivity, menu: GameMenu) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = { menu.page = GameMenu.Page.Main }) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.menu_back), tint = Text)
        }
        Text(stringResource(R.string.menu_controllers), Modifier.weight(1f), color = Text, fontWeight = FontWeight.Bold, fontSize = 22.sp)
        IconButton(onClick = { menu.close() }) { Icon(Icons.Default.Close, stringResource(R.string.menu_close), tint = Muted) }
    }
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        PlayersPanel(activity) { activity.uiControllersChanged() }
    }
}

/** The controllers as a line for the sidebar's list: how many are connected and whether two play. */
@Composable private fun padSummary(context: Context): String {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(700); tick++ } }
    val pads = remember(tick) { ExternalController.pads(context) }
    if (pads.isEmpty()) return stringResource(R.string.ctrl_none)
    val count = stringResource(R.string.menu_controllers_count, pads.size)
    return if (pads.count { it.role >= 0 } >= 2 || pads.any { it.role == ExternalController.PLAYER_TWO })
        count + " · " + stringResource(R.string.menu_controllers_two) else count
}

/**
 * The connected gamepads, each with the player it plays as. Shared by the controller screen and the
 * game's sidebar; [onChanged] runs after a choice has been saved so a running game picks it up now.
 * The touch screen is not listed: it always belongs to the first player.
 */
@Composable private fun PlayersPanel(context: Context, onChanged: () -> Unit) {
    var tick by remember { mutableIntStateOf(0) }
    // Plugging a pad in or out is not announced to Compose; look again every now and then.
    LaunchedEffect(Unit) { while (true) { delay(700); tick++ } }
    val pads = remember(tick) { ExternalController.pads(context) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (pads.isEmpty()) Text(stringResource(R.string.ctrl_players_none), color = Muted, fontSize = 13.sp)
        pads.forEach { pad ->
            PadRow(pad) { role ->
                ExternalController.setRole(context, pad.device, role)
                tick++
                onChanged()
            }
        }
        var rumble by remember { mutableStateOf(ExternalController.rumbleEnabled(context)) }
        ToggleRow(stringResource(R.string.ctrl_rumble), rumble, true) {
            rumble = it
            ExternalController.setRumbleEnabled(context, it)
            onChanged()
        }
        Text(stringResource(R.string.ctrl_rumble_note), color = Muted, fontSize = 12.sp)
        ActionButton(stringResource(R.string.ctrl_rumble_test), rumble, outlined = true) { Rumble.test(context) }
        val twoPlayers = pads.any { it.role == ExternalController.PLAYER_TWO }
        if (twoPlayers) Text(stringResource(R.string.ctrl_players_two_active), color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        else if (pads.isNotEmpty()) Text(stringResource(R.string.ctrl_players_hint_one), color = Muted, fontSize = 13.sp)
        Text(stringResource(R.string.ctrl_players_touch), color = Muted, fontSize = 12.sp)
    }
}

@Composable private fun PadRow(pad: ExternalController.Pad, onRole: (Int) -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Color(0xFF2A1C11)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.Gamepad, null, tint = if (pad.role >= 0) Accent else Muted, modifier = Modifier.size(24.dp))
            Text(pad.name, Modifier.weight(1f), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf(
                ExternalController.PLAYER_ONE to R.string.ctrl_player_one,
                ExternalController.PLAYER_TWO to R.string.ctrl_player_two,
                ExternalController.NOT_USED to R.string.ctrl_player_off
            ).forEach { (role, label) ->
                FilterChip(selected = pad.role == role, onClick = { onRole(role) }, label = { Text(stringResource(label), fontSize = 13.sp) })
            }
        }
    }
}

@Composable private fun SidebarItem(
    icon: ImageVector, title: String, subtitle: String? = null, highlighted: Boolean = false,
    chevron: Boolean = false, onClick: () -> Unit
) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp))
            .background(if (highlighted) Color(0xFF4B301F) else Color(0xFF2A1C11))
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

/** The frame-rate bar in the sidebar: the game follows it as it moves, and the choice is kept when it lets go. */
@Composable private fun SidebarFpsTarget(activity: RuntimeActivity) {
    var position by remember { mutableFloatStateOf(RuntimeOptions.sliderFromFps(activity.uiFpsTarget()).toFloat()) }
    val fps = RuntimeOptions.fpsFromSlider(Math.round(position))
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Color(0xFF2A1C11))
            .padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 2.dp)
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.Speed, null, tint = Text, modifier = Modifier.size(24.dp))
            Text(stringResource(R.string.settings_fps_target), Modifier.weight(1f), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
            Text(fpsLabel(fps), color = Accent, fontWeight = FontWeight.Bold, fontSize = 13.sp)
        }
        Slider(
            value = position,
            onValueChange = { position = it; activity.uiSetFpsTarget(RuntimeOptions.fpsFromSlider(Math.round(it))) },
            modifier = Modifier.fillMaxWidth().height(32.dp),
            valueRange = 0f..RuntimeOptions.SLIDER_UNLIMITED.toFloat(),
            steps = RuntimeOptions.SLIDER_UNLIMITED - 1,
            onValueChangeFinished = { activity.uiSaveFpsTarget(RuntimeOptions.fpsFromSlider(Math.round(position))) })
    }
}

@Composable private fun SidebarToggle(icon: ImageVector, title: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Color(0xFF2A1C11))
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
            IconButton(onClick = { activity.finish() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.common_back)) }
            Text(stringResource(R.string.ctrl_title), color = Text, fontWeight = FontWeight.Bold, fontSize = 21.sp)
        }
    }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            item { Text(stringResource(R.string.ctrl_intro), color = Muted) }
            item { Panel(stringResource(R.string.ctrl_players_title)) { PlayersPanel(activity) {} } }
            item {
                Panel(stringResource(R.string.ctrl_panel_device)) {
                    ActionButton(devices.getOrNull(activity.uiSelected()) ?: stringResource(R.string.ctrl_none), true, outlined = true) { chooseDevice = true }
                    ActionButton(stringResource(R.string.ctrl_refresh), true, outlined = true) { activity.uiRefresh(); tick++ }
                }
            }
            item {
                Panel(stringResource(R.string.ctrl_panel_mapping)) {
                    Text(activity.uiStatus(), color = Muted, fontSize = 14.sp)
                    ActionButton(stringResource(R.string.ctrl_auto), devices.isNotEmpty()) { activity.uiAutomatic(); tick++ }
                    if (activity.uiPending() >= 0) ActionButton(stringResource(R.string.ctrl_cancel_capture), true, outlined = true) { activity.uiCancel(); tick++ }
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
        title = { Text(stringResource(R.string.ctrl_choose_title)) },
        text = { Column { devices.forEachIndexed { index, name ->
            Text(name, Modifier.fillMaxWidth().clickable { activity.uiSelect(index); chooseDevice = false; tick++ }.padding(12.dp))
        } } },
        confirmButton = { TextButton(onClick = { chooseDevice = false }) { Text(stringResource(R.string.common_close)) } })
}

@Composable private fun RuntimeScreen(activity: RuntimeActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    Column(Modifier.fillMaxSize().background(Background).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Header(stringResource(R.string.runtime_eyebrow), if (activity.uiGraphics()) "RexGlue + Plume" else "Runtime ARM64", stringResource(R.string.runtime_subtitle))
        Panel(stringResource(R.string.result_panel)) { Text(activity.uiStatus(), color = Text, fontSize = 13.sp) }
        if (activity.uiGraphics()) AndroidView(factory = { SurfaceView(activity).also(activity::attachComposeSurface) }, modifier = Modifier.fillMaxWidth().weight(1f))
        else Spacer(Modifier.weight(1f))
        ActionButton(stringResource(R.string.common_back), true) { activity.finish() }
    }
}

// ---- First run ----------------------------------------------------------------------------------

/** Content scrolls in the middle; the step's actions stay on the bottom edge of the screen. */
@Composable private fun WelcomeScreen(activity: WelcomeActivity, model: WelcomeModel) {
    Box(Modifier.fillMaxSize().background(Background)) {
    AnimatedContent(
        targetState = model.step, label = "welcome-step",
        modifier = Modifier.fillMaxSize(),
        transitionSpec = { pageTransition(targetState > initialState) }
    ) { step ->
        // `step` (not model.step) so a leaving page keeps its own content and buttons while it fades out.
        Column(Modifier.fillMaxSize().systemBarsPadding()) {
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                Column(
                    Modifier.widthIn(max = 560.dp).verticalScroll(rememberScrollState()).padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(20.dp)
                ) {
                    when (step) {
                        WelcomeModel.INTRO -> WelcomeIntro()
                        WelcomeModel.NOTICE -> WelcomeNotice()
                        WelcomeModel.INSTALL -> WelcomeInstall(model)
                        WelcomeModel.DONE -> WelcomeDone(model)
                        else -> WelcomeFailed(model)
                    }
                }
            }
            Column(
                Modifier.align(Alignment.CenterHorizontally).widthIn(max = 560.dp).fillMaxWidth()
                    .padding(start = 24.dp, end = 24.dp, bottom = 20.dp, top = 8.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) { WelcomeActions(activity, step) }
        }
    }
    // Top right, above every step and outside the page animation.
    LanguageMenu(Modifier.align(Alignment.TopEnd).systemBarsPadding().padding(8.dp))
    }
}

@Composable private fun ColumnScope.WelcomeActions(activity: WelcomeActivity, step: Int) {
    when (step) {
        WelcomeModel.INTRO -> ActionButton(stringResource(R.string.welcome_continue), true) { activity.uiContinueFromIntro() }
        WelcomeModel.NOTICE -> {
            ActionButton(stringResource(R.string.btn_import_iso), true) { activity.uiPick(true) }
            ActionButton(stringResource(R.string.btn_import_xex), true, outlined = true) { activity.uiPick(false) }
        }
        WelcomeModel.INSTALL -> ActionButton(stringResource(R.string.common_cancel), true, outlined = true) { activity.uiCancelInstall() }
        WelcomeModel.DONE -> ActionButton(stringResource(R.string.btn_start), true) { activity.uiFinish() }
        else -> ActionButton(stringResource(R.string.btn_retry), true) { activity.uiBackToNotice() }
    }
}

@Composable private fun ColumnScope.WelcomeIntro() {
    Header(stringResource(R.string.welcome_intro_eyebrow), stringResource(R.string.welcome_intro_title), stringResource(R.string.welcome_intro_subtitle))
    Panel(null) {
        Text(stringResource(R.string.welcome_intro_body1),
            color = Text, fontSize = 15.sp)
        Text(stringResource(R.string.welcome_intro_body2),
            color = Muted, fontSize = 14.sp)
    }
}

@Composable private fun ColumnScope.WelcomeNotice() {
    Header(stringResource(R.string.notice_eyebrow), stringResource(R.string.notice_title), stringResource(R.string.notice_subtitle))
    Panel(null) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(Icons.Default.Warning, null, tint = Accent, modifier = Modifier.size(24.dp))
            Text(stringResource(R.string.notice_legal), color = Text, fontSize = 15.sp)
        }
        InfoRow(stringResource(R.string.notice_iso_title), stringResource(R.string.notice_iso_body))
        InfoRow(stringResource(R.string.notice_xex_title), stringResource(R.string.notice_xex_body))
        Text(stringResource(R.string.notice_time), color = Muted, fontSize = 13.sp)
    }
}

@Composable private fun ColumnScope.WelcomeInstall(model: WelcomeModel) {
    Header(stringResource(R.string.install_eyebrow), stringResource(R.string.install_title),
        if (model.wholeDisc) stringResource(R.string.install_sub_disc) else stringResource(R.string.install_sub_xex))
    Panel(null) {
        Text(model.message, color = Text, fontSize = 15.sp)
        if (model.fraction < 0f) LinearProgressIndicator(
            modifier = Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
            color = Accent, trackColor = Color(0xFF3E2C1F))
        else LinearProgressIndicator(
            progress = { model.fraction.coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
            color = Accent, trackColor = Color(0xFF3E2C1F))
        if (model.fraction >= 0f) Text("${(model.fraction.coerceIn(0f, 1f) * 100).toInt()}%", color = Accent, fontWeight = FontWeight.Bold)
        Text(stringResource(R.string.install_dont_close), color = Muted, fontSize = 13.sp)
    }
}

@Composable private fun ColumnScope.WelcomeDone(model: WelcomeModel) {
    Header(stringResource(R.string.done_eyebrow), stringResource(R.string.done_title), stringResource(R.string.done_subtitle))
    Panel(null) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.CheckCircle, null, tint = Accent, modifier = Modifier.size(32.dp))
            Text(
                if (model.wholeDisc) stringResource(R.string.done_body_disc)
                else stringResource(R.string.done_body_xex),
                color = Text, fontSize = 15.sp)
        }
    }
}

@Composable private fun ColumnScope.WelcomeFailed(model: WelcomeModel) {
    Header(stringResource(R.string.fail_eyebrow), stringResource(R.string.fail_title), stringResource(R.string.fail_subtitle))
    Panel(null) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(Icons.Default.Warning, null, tint = Accent, modifier = Modifier.size(24.dp))
            Text(model.error.ifEmpty { stringResource(R.string.fail_unknown) }, color = Text, fontSize = 15.sp)
        }
    }
}

// ---- Screen layout: saved layouts and the editor ------------------------------------------------------

/** The saved layouts: which exist and which one the game uses. The list screen shows and changes them. */
class LayoutListModel(private val context: Context) {
    var names by mutableStateOf(HudLayout.names(context))
    var selected by mutableStateOf(HudLayout.selectedName(context))

    fun refresh() {
        names = HudLayout.names(context)
        selected = HudLayout.selectedName(context)
    }

    fun use(name: String) {
        runCatching { HudLayout.select(context, name) }
        refresh()
    }

    fun delete(name: String) {
        runCatching { HudLayout.delete(context, name) }
        refresh()
    }

    /** 0 created, 1 no name, 2 the name is taken. The new layout starts as the built-in one and becomes the one in use. */
    fun create(raw: String): Int {
        val name = raw.trim().take(24)
        if (name.isEmpty()) return 1
        if (name.equals(HudLayout.DEFAULT, ignoreCase = true) || names.any { it.equals(name, ignoreCase = true) }) return 2
        runCatching { HudLayout.save(context, HudLayout.Profile(name)) }
        refresh()
        return 0
    }
}

/**
 * What the editor works on: a copy of one profile, the element picked, and whether the copy
 * differs from what is saved. Elements are moved in place, so [revision] is what tells Compose
 * to redraw.
 */
class LayoutEditorModel(private val context: Context, initial: String) {
    var profile by mutableStateOf(HudLayout.load(context, initial))
    var names by mutableStateOf(HudLayout.names(context))
    var picked by mutableStateOf<String?>(null)
    var revision by mutableIntStateOf(0)
    var dirty by mutableStateOf(false)
    /** The "save the changes?" question, raised by Back while there are unsaved ones. */
    var asking by mutableStateOf(false)

    fun touch() { revision++; dirty = true }

    /** False when this is the built-in profile, which is never overwritten: the caller asks for a name. */
    fun save(): Boolean {
        if (profile.isDefault()) return false
        runCatching { HudLayout.save(context, profile) }
        names = HudLayout.names(context)
        dirty = false
        return true
    }

    /** 0 saved, 1 no name, 2 the name is taken. */
    fun saveAs(raw: String): Int {
        val name = raw.trim().take(24)
        if (name.isEmpty()) return 1
        if (name.equals(HudLayout.DEFAULT, ignoreCase = true) || names.any { it.equals(name, ignoreCase = true) }) return 2
        profile = profile.copy(name)
        runCatching { HudLayout.save(context, profile) }
        names = HudLayout.names(context)
        dirty = false
        revision++
        return 0
    }

    fun resetPicked() {
        val id = picked ?: return
        val item = profile.item(id)
        item.placed = false
        item.scale = 1f
        item.visible = true
        touch()
    }
}

private val EDITOR_LABELS = mapOf(
    HudLayout.LT to "LT", HudLayout.RT to "RT", HudLayout.LB to "LB", HudLayout.RB to "RB",
    HudLayout.STICK_L to "L", HudLayout.STICK_R to "R", HudLayout.DPAD to "D-PAD",
    HudLayout.Y to "Y", HudLayout.X to "X", HudLayout.B to "B", HudLayout.A to "A",
    HudLayout.BACK to "BACK", HudLayout.START to "START")

private fun editorColor(id: String): Color = when (id) {
    HudLayout.A -> Color(0xFF5DBB2F)
    HudLayout.B -> Color(0xFFD8272A)
    HudLayout.X -> Color(0xFF1E73BE)
    HudLayout.Y -> Color(0xFFF5B800)
    HudLayout.FPS -> Color(0xFF78FF78)
    HudLayout.TEMP -> Color(0xFFFFC46E)
    else -> Color.White
}

@Composable private fun layoutProfileLabel(name: String): String =
    if (name == HudLayout.DEFAULT) stringResource(R.string.layout_default) else name

@Composable private fun layoutItemName(id: String): String = when (id) {
    HudLayout.LT -> stringResource(R.string.layout_item_lt)
    HudLayout.RT -> stringResource(R.string.layout_item_rt)
    HudLayout.LB -> stringResource(R.string.layout_item_lb)
    HudLayout.RB -> stringResource(R.string.layout_item_rb)
    HudLayout.STICK_L -> stringResource(R.string.layout_item_stick_l)
    HudLayout.STICK_R -> stringResource(R.string.layout_item_stick_r)
    HudLayout.DPAD -> stringResource(R.string.layout_item_dpad)
    HudLayout.BACK -> stringResource(R.string.layout_item_back)
    HudLayout.START -> stringResource(R.string.layout_item_start)
    HudLayout.FPS -> stringResource(R.string.settings_fps)
    HudLayout.TEMP -> stringResource(R.string.settings_temp)
    else -> stringResource(R.string.layout_item_face, id.uppercase())
}

/** The element under a touch, the one drawn last (on top) first. */
private fun pickElement(model: LayoutEditorModel, at: androidx.compose.ui.geometry.Offset, size: IntSize): String? {
    if (size.width <= 0) return null
    val slop = minOf(size.width, size.height) / 40f
    for (id in HudLayout.IDS.reversed()) {
        if (HudLayout.shape(model.profile, id, size.width, size.height).contains(at.x, at.y, slop)) return id
    }
    return null
}

private fun moveElement(model: LayoutEditorModel, id: String, by: androidx.compose.ui.geometry.Offset, size: IntSize) {
    if (size.width <= 0) return
    val shape = HudLayout.shape(model.profile, id, size.width, size.height)
    val item = model.profile.item(id)
    item.placed = true
    item.x = HudLayout.clamp((shape.cx + by.x) / size.width, 0f, 1f)
    item.y = HudLayout.clamp((shape.cy + by.y) / size.height, 0f, 1f)
    model.touch()
}

/** A name for a new layout; asks again with the reason while the answer is not usable. */
@Composable private fun LayoutNameDialog(
    title: String, initial: String, confirmLabel: String, onDismiss: () -> Unit, onConfirm: (String) -> Int
) {
    var name by remember { mutableStateOf(initial) }
    var problem by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                OutlinedTextField(name, { name = it.take(24); problem = false }, singleLine = true,
                    label = { Text(stringResource(R.string.layout_name_hint)) })
                if (problem) Text(stringResource(R.string.layout_name_taken), color = Color(0xFFFF8A80), fontSize = 12.sp)
            }
        },
        confirmButton = { TextButton({ if (onConfirm(name) != 0) problem = true }) { Text(confirmLabel) } },
        dismissButton = { TextButton(onDismiss) { Text(stringResource(R.string.common_cancel)) } })
}

@Composable private fun LayoutListScreen(activity: LayoutEditorActivity, model: LayoutListModel) {
    var creating by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<String?>(null) }
    Column(Modifier.fillMaxSize().background(Background).systemBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton({ activity.finish() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.common_back), tint = Text) }
            Text(stringResource(R.string.layouts_title), color = Text, fontWeight = FontWeight.Bold, fontSize = 21.sp,
                modifier = Modifier.weight(1f))
            Button({ creating = true }, shape = RoundedCornerShape(12.dp)) { Text(stringResource(R.string.layouts_new)) }
        }
        LazyColumn(
            Modifier.fillMaxSize(), contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            items(model.names, key = { it }) { name ->
                val active = name == model.selected
                Card(
                    colors = CardDefaults.cardColors(containerColor = if (active) Color(0xFF2A2018) else Surface),
                    shape = RoundedCornerShape(16.dp), modifier = Modifier.fillMaxWidth().clickable { model.use(name) }
                ) {
                    Row(Modifier.padding(horizontal = 14.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        RadioButton(selected = active, onClick = { model.use(name) })
                        Column(Modifier.weight(1f)) {
                            Text(layoutProfileLabel(name), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 16.sp,
                                maxLines = 1, overflow = TextOverflow.Ellipsis)
                            if (name == HudLayout.DEFAULT) Text(stringResource(R.string.layouts_builtin), color = Muted, fontSize = 12.sp)
                            else if (active) Text(stringResource(R.string.layouts_active), color = Accent, fontSize = 12.sp)
                        }
                        // The built-in layout is a fixed starting point: it can be used and copied, not edited.
                        if (name != HudLayout.DEFAULT) {
                            OutlinedButton({ activity.edit(name) }, shape = RoundedCornerShape(12.dp),
                                contentPadding = PaddingValues(horizontal = 14.dp)) { Text(stringResource(R.string.layouts_edit)) }
                            OutlinedButton({ deleting = name }, shape = RoundedCornerShape(12.dp),
                                contentPadding = PaddingValues(horizontal = 14.dp)) { Text(stringResource(R.string.layout_delete)) }
                        }
                    }
                }
            }
        }
    }
    if (creating) LayoutNameDialog(
        stringResource(R.string.layouts_new), "", stringResource(R.string.layouts_create),
        onDismiss = { creating = false },
        onConfirm = { entered ->
            val result = model.create(entered)
            if (result == 0) { creating = false; activity.edit(entered.trim().take(24)) }
            result
        })
    deleting?.let { name ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text(stringResource(R.string.layout_delete_title)) },
            text = { Text(stringResource(R.string.layout_delete_message, name)) },
            confirmButton = { TextButton({ deleting = null; model.delete(name) }) { Text(stringResource(R.string.layout_delete)) } },
            dismissButton = { TextButton({ deleting = null }) { Text(stringResource(R.string.common_cancel)) } })
    }
}

@Composable private fun LayoutEditorScreen(activity: LayoutEditorActivity, model: LayoutEditorModel) {
    var size by remember { mutableStateOf(IntSize.Zero) }
    var dragging by remember { mutableStateOf<String?>(null) }
    var askName by remember { mutableStateOf(false) }
    // The panel can be moved out of the way of whatever is being placed; it starts in the middle,
    // which the pad's own layout leaves free.
    var panelShift by remember { mutableStateOf(androidx.compose.ui.geometry.Offset.Zero) }
    val context = LocalContext.current
    val density = LocalDensity.current
    val labels = remember { android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = android.graphics.Paint.Align.CENTER
        isFakeBoldText = true
        color = android.graphics.Color.WHITE
    } }
    // What the temperature readout will show on this device (or an example where nothing can be read).
    val sampleTemp = remember { runCatching { ThermalReader(context).read() }.getOrDefault("").ifEmpty { "BAT 36°" } }

    Box(
        Modifier.fillMaxSize().background(Color(0xFF0C0704)).onSizeChanged { size = it }
            .pointerInput(Unit) { detectTapGestures { p -> model.picked = pickElement(model, p, size) } }
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = { p ->
                        dragging = pickElement(model, p, size)
                        if (dragging != null) model.picked = dragging
                    },
                    onDrag = { change, amount ->
                        change.consume()
                        dragging?.let { moveElement(model, it, amount, size) }
                    },
                    onDragEnd = { dragging = null },
                    onDragCancel = { dragging = null })
            }
    ) {
        Canvas(Modifier.fillMaxSize()) {
            model.revision  // read: a moved element redraws
            val w = this.size.width.toInt()
            val h = this.size.height.toInt()
            val canvas = drawContext.canvas.nativeCanvas
            val unit = minOf(w, h) / 10f
            drawRect(Color(0xFF150D07))
            for (id in HudLayout.IDS) {
                val shape = HudLayout.shape(model.profile, id, w, h)
                val shown = HudLayout.visible(model.profile, id)
                val tint = editorColor(id)
                val readout = HudLayout.isReadout(id)
                // Each element is drawn with its own opacity, as the game will draw it.
                val alpha = (if (shown) 1f else 0.25f) * HudLayout.opacity(model.profile, id)
                val outline = if (id == model.picked) Color(0xFFFF9B45) else tint.copy(alpha = 0.8f * alpha)
                val strokeWidth = if (id == model.picked) unit * 0.09f else unit * 0.05f
                if (readout) {
                    val text = if (id == HudLayout.FPS) "FPS 30.0" else sampleTemp
                    labels.color = tint.copy(alpha = alpha).toArgb()
                    labels.textSize = unit * 0.34f * (model.profile.items[id]?.scale ?: 1f)
                    canvas.drawText(text, shape.cx, shape.cy + labels.textSize / 3f, labels)
                    drawRoundRect(
                        outline, androidx.compose.ui.geometry.Offset(shape.cx - shape.hw, shape.cy - shape.hh),
                        androidx.compose.ui.geometry.Size(shape.hw * 2, shape.hh * 2),
                        androidx.compose.ui.geometry.CornerRadius(unit * 0.15f), style = Stroke(strokeWidth))
                    continue
                }
                val fill = tint.copy(alpha = 0.34f * alpha)
                if (shape.circle) {
                    drawCircle(fill, shape.r, androidx.compose.ui.geometry.Offset(shape.cx, shape.cy))
                    drawCircle(outline, shape.r, androidx.compose.ui.geometry.Offset(shape.cx, shape.cy), style = Stroke(strokeWidth))
                } else {
                    val topLeft = androidx.compose.ui.geometry.Offset(shape.cx - shape.hw, shape.cy - shape.hh)
                    val extent = androidx.compose.ui.geometry.Size(shape.hw * 2, shape.hh * 2)
                    val corner = androidx.compose.ui.geometry.CornerRadius(minOf(shape.hh, shape.hw) * 0.7f)
                    drawRoundRect(fill, topLeft, extent, corner)
                    drawRoundRect(outline, topLeft, extent, corner, style = Stroke(strokeWidth))
                }
                labels.color = Color.White.copy(alpha = 0.95f * alpha).toArgb()
                labels.textSize = unit * 0.36f
                canvas.drawText(EDITOR_LABELS[id] ?: "", shape.cx, shape.cy + labels.textSize / 3f, labels)
            }
        }

        val maxPanelHeight = with(density) { (size.height * 0.8f).toDp() }
        Card(
            Modifier.align(Alignment.Center)
                .offset { androidx.compose.ui.unit.IntOffset(panelShift.x.toInt(), panelShift.y.toInt()) }
                .widthIn(max = 292.dp).heightIn(max = maxPanelHeight)
                // A touch on the panel is the panel's, not a pick of the element under it.
                .pointerInput(Unit) { detectTapGestures { } },
            colors = CardDefaults.cardColors(containerColor = Color(0xE61E140C)), shape = RoundedCornerShape(16.dp)
        ) {
            Column(Modifier.padding(horizontal = 10.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                // The title: drag here to move the panel.
                Row(
                    Modifier.fillMaxWidth().pointerInput(Unit) {
                        detectDragGestures { change, amount ->
                            change.consume()
                            panelShift = androidx.compose.ui.geometry.Offset(panelShift.x + amount.x, panelShift.y + amount.y)
                        }
                    },
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.layout_title), color = Text, fontWeight = FontWeight.Bold, fontSize = 15.sp)
                        Text(layoutProfileLabel(model.profile.name), color = Accent, fontSize = 12.sp, maxLines = 1,
                            overflow = TextOverflow.Ellipsis)
                    }
                }
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    val picked = model.picked
                    if (picked == null) {
                        Text(stringResource(R.string.layout_hint), color = Muted, fontSize = 11.sp)
                    } else {
                        val item = model.profile.item(picked)
                        model.revision  // the sliders follow resets
                        Text(layoutItemName(picked), color = Text, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(stringResource(R.string.layout_size), color = Muted, fontSize = 12.sp, modifier = Modifier.width(64.dp))
                            Slider(item.scale, { item.scale = it; model.touch() },
                                valueRange = HudLayout.MIN_SCALE..HudLayout.MAX_SCALE, modifier = Modifier.weight(1f).height(28.dp))
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(stringResource(R.string.layout_opacity), color = Muted, fontSize = 12.sp, modifier = Modifier.width(64.dp))
                            Slider(item.opacity, { item.opacity = it; model.touch() },
                                valueRange = HudLayout.MIN_OPACITY..1f, modifier = Modifier.weight(1f).height(28.dp))
                        }
                        // A pad control cannot be hidden; the FPS and temperature readouts can.
                        if (HudLayout.isReadout(picked)) Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(stringResource(R.string.layout_visible), color = Muted, fontSize = 12.sp, modifier = Modifier.width(64.dp))
                            Switch(item.visible, { item.visible = it; model.touch() }, modifier = Modifier.scale(0.8f))
                        }
                        OutlinedButton({ model.resetPicked() }, Modifier.fillMaxWidth().height(34.dp), shape = RoundedCornerShape(10.dp),
                            contentPadding = PaddingValues(horizontal = 8.dp)) {
                            Text(stringResource(R.string.layout_reset_item), fontSize = 12.sp, maxLines = 1)
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(top = 4.dp)) {
                        Button({ if (!model.save()) askName = true }, Modifier.weight(1f).height(36.dp),
                            enabled = model.dirty, shape = RoundedCornerShape(10.dp),
                            contentPadding = PaddingValues(horizontal = 4.dp)) { Text(stringResource(R.string.layout_save), fontSize = 12.sp, maxLines = 1) }
                        OutlinedButton({ askName = true }, Modifier.weight(1f).height(36.dp), shape = RoundedCornerShape(10.dp),
                            contentPadding = PaddingValues(horizontal = 4.dp)) { Text(stringResource(R.string.layout_save_as), fontSize = 12.sp, maxLines = 1) }
                        OutlinedButton({ activity.leave() }, Modifier.weight(1f).height(36.dp), shape = RoundedCornerShape(10.dp),
                            contentPadding = PaddingValues(horizontal = 4.dp)) { Text(stringResource(R.string.layout_done), fontSize = 12.sp, maxLines = 1) }
                    }
                }
            }
        }
    }

    if (askName) LayoutNameDialog(
        stringResource(R.string.layout_save_as_title), if (model.profile.isDefault()) "" else model.profile.name,
        stringResource(R.string.layout_save), onDismiss = { askName = false },
        onConfirm = { entered -> model.saveAs(entered).also { if (it == 0) askName = false } })
    if (model.asking) AlertDialog(
        onDismissRequest = { model.asking = false },
        title = { Text(stringResource(R.string.layout_unsaved_title)) },
        text = { Text(stringResource(R.string.layout_unsaved_message)) },
        confirmButton = {
            TextButton({
                model.asking = false
                if (model.save()) activity.finish() else askName = true
            }) { Text(stringResource(R.string.layout_save)) }
        },
        dismissButton = {
            Row {
                TextButton({ model.asking = false }) { Text(stringResource(R.string.common_cancel)) }
                TextButton({ model.asking = false; activity.finish() }) { Text(stringResource(R.string.layout_discard)) }
            }
        })
}
