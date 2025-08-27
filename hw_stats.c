// hw_stats_with_cpu_mem.c
// GTK4 + Cairo temperature, CPU and Memory graphs with retained history and labels.
// Build: gcc $(pkg-config --cflags gtk4) -o hw_stats hw_stats_with_cpu_mem.c $(pkg-config --libs gtk4)

#include <gtk/gtk.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HISTORY_SIZE 200    // how many points we keep
#define GRAPH_HEIGHT 100    // single graph pixel height
#define GRAPH_MARGIN 10     // left/right margin
#define V_SPACING 60        // vertical spacing between graphs
#define MEM_LINES 6         // lines to read from /proc/meminfo

/* UI widgets */
static GtkWidget *label_perf;   // performance label (top)
static GtkWidget *label_mem;    // memory label (below perf)
static GtkWidget *drawing_area; // cairo drawing area (graph)

/* Data */
static double temp_history[HISTORY_SIZE];
static double cpu_history[HISTORY_SIZE];
static double mem_history[HISTORY_SIZE];
static int history_count = 0; // how many valid samples (<= HISTORY_SIZE)

/* For CPU usage calculation */
static unsigned long long prev_total = 0;
static unsigned long long prev_idle = 0;
static gboolean cpu_prev_ready = FALSE;

/* --- Helpers: read temperature (vagile, supports vcgencmd fallback) --- */
static int parse_temp_from_string(const char *s, double *out_temp) {
    const char *p = s;
    int found_digit = 0;
    double val = 0.0;
    double frac = 0.0;
    double scale = 1.0;
    int in_frac = 0;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            found_digit = 1;
            int d = *p - '0';
            if (!in_frac) {
                val = val * 10.0 + d;
            } else {
                frac = frac * 10.0 + d;
                scale *= 10.0;
            }
        } else if (*p == '.' && found_digit && !in_frac) {
            in_frac = 1;
        } else if (found_digit) {
            break;
        }
        p++;
    }
    if (!found_digit) return 0;
    if (scale > 1.0) val += frac / scale;
    *out_temp = val;
    return 1;
}

static double read_temperature() {
    double temp = 0.0;
    FILE *fp = popen("vcgencmd measure_temp 2>/dev/null", "r");
    if (fp) {
        char buf[128];
        if (fgets(buf, sizeof(buf), fp) != NULL) {
            if (parse_temp_from_string(buf, &temp)) {
                pclose(fp);
                return temp;
            }
        }
        pclose(fp);
    }

    // fallback: read CPU thermal zone (common on Linux)
    fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp) {
        long milli = 0;
        if (fscanf(fp, "%ld", &milli) == 1) {
            temp = milli / 1000.0;
            fclose(fp);
            return temp;
        }
        fclose(fp);
    }

    return 0.0;
}

/* --- Read CPU usage: parse /proc/stat and compute percentage 0..100 --- */
static double read_cpu_usage() {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0;
    char line[512];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);

    // Parse the first line starting with "cpu"
    // Format: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0, guest=0, guest_nice=0;
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
    if (n < 4) return 0.0;

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + system + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;

    double usage = 0.0;
    if (cpu_prev_ready) {
        unsigned long long totald = total - prev_total;
        unsigned long long idled = idle_all - prev_idle;
        if (totald > 0) {
            usage = (double)(totald - idled) * 100.0 / (double)totald;
        }
    }

    prev_total = total;
    prev_idle = idle_all;
    cpu_prev_ready = TRUE;
    return usage; // 0..100
}

/* --- Read memory usage percentage using /proc/meminfo --- */
static double read_mem_usage_percent() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0;
    char line[256];
    unsigned long long mem_total = 0;
    unsigned long long mem_avail = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %llu kB", &mem_avail) == 1) continue;
        // fallback if MemAvailable isn't present: compute from MemFree + Buffers + Cached
        unsigned long long v;
        if (mem_avail == 0 && sscanf(line, "MemFree: %llu kB", &v) == 1) mem_avail += v;
        if (mem_avail == 0 && sscanf(line, "Buffers: %llu kB", &v) == 1) mem_avail += v;
        if (mem_avail == 0 && sscanf(line, "Cached: %llu kB", &v) == 1) mem_avail += v;
    }
    fclose(fp);
    if (mem_total == 0) return 0.0;
    if (mem_avail > mem_total) mem_avail = mem_total; // sanity
    double used_percent = (1.0 - ((double)mem_avail / (double)mem_total)) * 100.0;
    if (used_percent < 0.0) used_percent = 0.0;
    if (used_percent > 100.0) used_percent = 100.0;
    return used_percent;
}

/* --- Read a few lines from /proc/meminfo to display --- */
static void build_mem_string(char *out, size_t outsz) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        snprintf(out, outsz, "Memory: unable to open /proc/meminfo\n");
        return;
    }
    char line[256];
    int count = 0;
    out[0] = '\0';
    while (count < MEM_LINES && fgets(line, sizeof(line), fp)) {
        size_t L = strlen(line);
        if (L && line[L-1] == '\n') line[L-1] = '\0';
        strncat(out, "  ", outsz - strlen(out) - 1);
        strncat(out, line, outsz - strlen(out) - 1);
        strncat(out, "\n", outsz - strlen(out) - 1);
        count++;
    }
    fclose(fp);
    // Add CPU stats in /proc/stat (first line)
    FILE *cpu_fp = fopen("/proc/stat", "r");
    if (cpu_fp) {
        char cpu_line[256];
        if (fgets(cpu_line, sizeof(cpu_line), cpu_fp)) {
            size_t len = strlen(cpu_line);
            if (len && cpu_line[len - 1] == '\n') cpu_line[len - 1] = '\0';
            strncat(out, "  ", outsz - strlen(out) - 1);
            strncat(out, "Cpu:", 4);
            strncat(out, cpu_line+3, 10); // small snippet
        }
        fclose(cpu_fp);
    } else {
        strncat(out, "  CPU: unable to open /proc/stat\n", outsz - strlen(out) - 1);
    }
}

/* --- Add new samples, keep history (scroll left) --- */
static void shift_and_add(double *arr, double value) {
    if (history_count < HISTORY_SIZE) {
        arr[history_count] = value;
        return;
    }
    memmove(&arr[0], &arr[1], sizeof(double) * (HISTORY_SIZE - 1));
    arr[HISTORY_SIZE - 1] = value;
}

static void add_samples(double t, double cpu, double mem) {
    if (history_count < HISTORY_SIZE) history_count++;
    shift_and_add(temp_history, t);
    shift_and_add(cpu_history, cpu);
    shift_and_add(mem_history, mem);
}

/* --- Draw a single graph in given rectangle --- */
static void draw_single_graph(cairo_t *cr, int g_x, int g_y, int g_w, int g_h,
                              const char *title, double *history, int points,
                              double minv, double maxv, double r, double g, double b) {
    // background for this graph (subtle)
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_rectangle(cr, g_x, g_y, g_w, g_h);
    cairo_fill(cr);

    // border
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_rectangle(cr, g_x - 1, g_y - 1, g_w + 2, g_h + 2);
    cairo_stroke(cr);

    // horizontal grid
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    for (int i = 0; i <= 4; ++i) {
        double yy = g_y + (g_h * i / 4.0);
        cairo_move_to(cr, g_x, yy);
        cairo_line_to(cr, g_x + g_w, yy);
    }
    cairo_stroke(cr);

    // axes labels (min/max)
    char lab[64];
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr, 1, 1, 1);
    snprintf(lab, sizeof(lab), "%.1f", maxv);
    cairo_move_to(cr, g_x + g_w + 6, g_y + 10);
    cairo_show_text(cr, lab);
    snprintf(lab, sizeof(lab), "%.1f", minv);
    cairo_move_to(cr, g_x + g_w + 6, g_y + g_h);
    cairo_show_text(cr, lab);

    // polyline
    if (points > 0) {
        int pts = points;
        double step_x = (double)g_w / (double)(HISTORY_SIZE - 1);
        for (int i = 0; i < pts; ++i) {
            double x = (pts > 1) ? (g_x + ((double)i * g_w) / (pts - 1)) : (g_x + g_w);
            double normalized = (history[i] - minv) / (maxv - minv);
            if (normalized < 0.0) normalized = 0.0;
            if (normalized > 1.0) normalized = 1.0;
            double y = g_y + (g_h - normalized * g_h);
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);

        // latest point marker
        double latest_x = (pts > 1) ? (g_x + ((double)(pts - 1) * g_w) / (pts - 1)) : (g_x + g_w);
        double latest_y = g_y + (g_h - ((history[pts - 1] - minv) / (maxv - minv)) * g_h);
        cairo_arc(cr, latest_x, latest_y, 3.0, 0, 2 * G_PI);
        cairo_fill(cr);
    }

    // title
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, g_x, g_y - 6);
    cairo_show_text(cr, title);
}

/* --- Draw graph with Cairo (three stacked graphs) --- */
static void draw_graph_cairo(cairo_t *cr, int width, int height) {
    // background
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.06);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    int g_x = GRAPH_MARGIN;
    int g_w = width - 2 * GRAPH_MARGIN;

    // arrange three graphs from top-to-bottom near bottom area with spacing
    int total_h = GRAPH_HEIGHT * 3 + V_SPACING * 2;
    int start_y = height - total_h - 8; // 8 px from bottom
    if (start_y < 30) start_y = 30; // ensure some room for labels

    int points = history_count > 0 ? history_count : 1;

    // Temperature graph
    draw_single_graph(cr, g_x, start_y, g_w, GRAPH_HEIGHT, "Temperature (°C)", temp_history, points, 30.0, 85.0, 0.1, 0.9, 0.2);

    // CPU graph
    int cpu_y = start_y + GRAPH_HEIGHT + V_SPACING;
    draw_single_graph(cr, g_x, cpu_y, g_w, GRAPH_HEIGHT, "CPU Usage (%)", cpu_history, points, 0.0, 100.0, 0.2, 0.6, 0.95);

    // Memory graph
    int mem_y = cpu_y + GRAPH_HEIGHT + V_SPACING;
    draw_single_graph(cr, g_x, mem_y, g_w, GRAPH_HEIGHT, "Memory Used (%)", mem_history, points, 0.0, 100.0, 0.95, 0.5, 0.1);
}

/* --- GTK draw callback for drawing area --- */
static void on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    draw_graph_cairo(cr, width, height);
}

/* --- The periodic update function (every second) --- */
static gboolean update_stats(gpointer user_data) {
    // Read sensors
    double t = read_temperature();
    if (t <= 0.0) t = 0.0; // safe fallback
    double cpu = read_cpu_usage();
    double mem = read_mem_usage_percent();

    add_samples(t, cpu, mem);

    // Update performance label
    char perf_buf[512];
    snprintf(perf_buf, sizeof(perf_buf),
             "<span size='large' weight='bold'>Performance Data:</span>\n  Temperature: %.2f °C\n  CPU: %.1f %%\n  Memory Used: %.1f %%",
             t, cpu, mem);
    gtk_label_set_markup(GTK_LABEL(label_perf), perf_buf);

    // Update memory label (verbose meminfo)
    char mem_buf[2048];
    build_mem_string(mem_buf, sizeof(mem_buf));
    char mem_out[2200];
    snprintf(mem_out, sizeof(mem_out), "<span size='large' weight='bold'>Memory Data:</span>\n%s", mem_buf);
    gtk_label_set_markup(GTK_LABEL(label_mem), mem_out);

    // Trigger redraw
    gtk_widget_queue_draw(drawing_area);

    return G_SOURCE_CONTINUE;
}

/* --- Activate: build UI --- */
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "HW_Stats (GTK4 + Cairo)");
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 600);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_window_set_child(GTK_WINDOW(window), vbox);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);

    label_perf = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label_perf), 0.0);
    gtk_label_set_use_markup(GTK_LABEL(label_perf), TRUE);
    gtk_widget_set_hexpand(label_perf, TRUE);

    label_mem = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label_mem), 0.0);
    gtk_label_set_use_markup(GTK_LABEL(label_mem), TRUE);
    gtk_label_set_wrap(GTK_LABEL(label_mem), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label_mem), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(label_mem, TRUE);

    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, -1, GRAPH_HEIGHT * 3 + V_SPACING * 2 + 60);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), (GtkDrawingAreaDrawFunc)on_draw, NULL, NULL);

    gtk_box_append(GTK_BOX(vbox), label_perf);
    gtk_box_append(GTK_BOX(vbox), label_mem);
    gtk_box_append(GTK_BOX(vbox), drawing_area);

    // Initialize histories with safe defaults
    double init_t = read_temperature();
    if (init_t <= 0.0) init_t = 40.0;
    double init_cpu = 0.0;
    double init_mem = read_mem_usage_percent();
    if (init_mem < 1.0) init_mem = 10.0;
    for (int i = 0; i < HISTORY_SIZE; ++i) {
        temp_history[i] = init_t;
        cpu_history[i] = init_cpu;
        mem_history[i] = init_mem;
    }
    history_count = HISTORY_SIZE;

    // Start periodic updates every second
    g_timeout_add_seconds(1, update_stats, NULL);

    gtk_window_present(GTK_WINDOW(window));
}

/* --- main --- */
int main(int argc, char **argv) {
    gtk_init();

    GtkApplication *app = gtk_application_new("org.example.hwstats", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
