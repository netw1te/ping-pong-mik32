#include "ping_pong.h"

#include <stdlib.h>

#include "scr1_timer_delay.h"
#include "ping_pong_buttons.h"
#include "ping_pong_sound.h"
#include "mik32_hal_gpio.h"
#include "ssd1306_gfx.h"
#include "sound.h"

#define PLATFORM_WIDTH      (SSD1306_WIDTH / 32)
#define PLATFORM_HEIGHT     (SSD1306_HEIGHT / 4)
#define PLAYER_1_START_XPOS (0)
#define PLAYER_1_START_YPOS (24)
#define PLAYER_2_START_XPOS (124)
#define PLAYER_2_START_YPOS (24)
#define PLAYER_SPEED        (2)

#define ORB_START_XPOS (64)
#define ORB_START_YPOS (32)
#define ORB_RADIUS     (2)
#define ORB_SPEED_MIN  (1)
#define ORB_SPEED_MAX  (2)

// Ускорение шарика: к случайной скорости (ORB_SPEED_MIN..ORB_SPEED_MAX) после каждого отбитого платформой
// удара прибавляется hits / ORB_HITS_PER_SPEEDUP, но скорость по X не выше ORB_SPEED_CAP.
// Потолок 3 выбран потому, что зона отбития у платформы шириной 4 пикселя:
// при шаге больше 4 шарик мог бы "перепрыгнуть" платформу.
#define ORB_SPEED_CAP        (3)
#define ORB_HITS_PER_SPEEDUP (3)

#define ORB_SPEED_RAND() (ORB_SPEED_MIN + rand() % (ORB_SPEED_MAX - ORB_SPEED_MIN + 1))
#define DIRECTION_RAND() (0 + rand() % (2 - 0 + 1))

// Сложность бота:
//   EASY   - старый бот: ездит вверх-вниз и не смотрит на шарик;
//   NORMAL и HARD - бот следит за шариком, пока тот летит к нему, но целится не точно в центр платформы:
//   на каждый подлёт выбирается случайная ошибка от -N до +N пикселей, где N - константа ниже.
// Половина платформы = PLATFORM_HEIGHT / 2 = 8 пикселей, поэтому ошибка больше 8 означает промах.
// Чем больше N, тем чаще бот промахивается (легче играть); меньше - бот сильнее.
#define ROBOT_NORMAL_AIM_ERROR_MAX (14)
#define ROBOT_HARD_AIM_ERROR_MAX   (7)
// Если цель ближе этого числа пикселей, бот стоит на месте (не дёргается из-за 1 пикселя)
#define ROBOT_DEAD_ZONE     (1)

typedef struct _Player
{
  int8_t x;
  int8_t y;
} Player;

typedef struct _Robot
{
  Player coordinates;
  bool direction;     // только для ROBOT_EASY: куда сейчас едет (true - вниз)
  int8_t aim_offset;  // на сколько пикселей центр платформы смещён от центра шарика (ошибка прицеливания)
  bool tracking;      // шарик сейчас летит к роботу; нужно, чтобы выбрать новую ошибку один раз за подлёт
} Robot;

typedef struct _Orb
{
  int16_t x;            // int16_t, т.к. при скорости 3 у правого края int8_t переполнился бы
  int16_t y;
  int8_t x_speed;
  int8_t y_speed;
  bool x_direction;
  bool y_direction;
  uint8_t hits;         // сколько раз шарик отбит платформами в текущем розыгрыше
} Orb;

static struct Gameplay
{
  enum Mode
  {
    PLAYER_VS_PLAYER,
    PLAYER_VS_ROBOT,
    SETTINGS
  } mode;

  enum Settings
  {
    MAX_SCORE,
    DIFFICULTY,
    MUSIC,
    GO_BACK
  } settings;

  enum Difficulty
  {
    ROBOT_EASY,
    ROBOT_NORMAL,
    ROBOT_HARD
  } difficulty;

  struct Counter
  {
    int8_t player_1_score;
    int8_t player_2_score;
    char score_buf[3];
  } counter;

  enum Music music;
  uint8_t max_score;
  bool is_game_over;
  bool player_1_won;

  bool is_paused;
  bool pause_button_was_pressed;
} gameplay;


static Player createPlayer(int8_t x_pos, int8_t y_pos);
static Robot createRobot(int8_t x_pos, int8_t y_pos, bool direction);
static Orb createOrb(int16_t x_pos, int16_t y_pos, int8_t x_speed, int8_t y_speed, bool x_dir, bool y_dir);

static void drawOrb(Orb * orb);
static void drawPlatform(Player * player);

static void showMenu(void) __attribute__((section(".ram_text")));
static void chooseMode(void);
static void showSettings(void);
static void settingsMode(void);
static void nextDifficulty(void);
static void initGraphics(void);
static void showGameOver(void);
static void incScorePlayer_1();
static void incScorePlayer_2();
static void resetCounter();

static void changeOrbPosition(Orb * orb) __attribute__((section(".ram_text")));
static void bounceOrbFromPlatform(Orb * orb, bool new_x_direction) __attribute__((section(".ram_text")));
static bool changeOrbDirIfCollisions(Orb * orb, Player * player_1, Player * player_2) __attribute__((section(".ram_text")));
static void changePlayersPosIfButtonsPressed(Player * player_1, Player * player_2) __attribute__((section(".ram_text")));
static void moveRobotZigzag(Robot * robot) __attribute__((section(".ram_text")));
static void moveRobotTracking(Robot * robot, const Orb * orb, int8_t aim_error_max) __attribute__((section(".ram_text")));
static void moveRobot(Robot * robot, const Orb * orb) __attribute__((section(".ram_text")));
static void changePlayerPosIfButtonPressedAndManageRobot(Player * player, Robot * robot, const Orb * orb) __attribute__((section(".ram_text")));
static bool renderPauseIfNeeded(
    Orb *orb,
    Player *player_1,
    Player *player_2
) __attribute__((section(".ram_text")));

void pingpong(void)
{
  gameplay.max_score = 3;
  gameplay.music = RETRO;
  gameplay.difficulty = ROBOT_NORMAL;

  for (;;)
  {
    gameplay.mode = PLAYER_VS_PLAYER;
    chooseMode();

    gameplay.is_paused = false;
    gameplay.pause_button_was_pressed = false;

    SSD1306_ClearDisplay();

    if (gameplay.mode == PLAYER_VS_PLAYER)
    {
      for (;;)
      {
        if (gameplay.is_game_over)
        {
          gameplay.is_game_over = false;
          resetCounter();
          showGameOver();
          break;
        }
        Player player_1 = createPlayer(PLAYER_1_START_XPOS, PLAYER_1_START_YPOS);
        Player player_2 = createPlayer(PLAYER_2_START_XPOS, PLAYER_2_START_YPOS);

        Orb orb = createOrb(ORB_START_XPOS, ORB_START_YPOS, ORB_SPEED_RAND(), ORB_SPEED_RAND(), DIRECTION_RAND(), DIRECTION_RAND());

        initGraphics();
        SSD1306_Display();
        SCR1_Timer_Delay(1500000);
        for (;;)
        {
          if (renderPauseIfNeeded(&orb, &player_1, &player_2))
          {
              SCR1_Timer_Delay(30000);
              continue;
          }
          SSD1306_ClearDisplay();
          changeOrbPosition(&orb);
          if (changeOrbDirIfCollisions(&orb, &player_1, &player_2))
          {
            SCR1_Timer_Delay(1500000);
            break;
          }
          changePlayersPosIfButtonsPressed(&player_1, &player_2);

          drawPlatform(&player_1);
          drawPlatform(&player_2);
          drawOrb(&orb);
          SSD1306_Display();
        }
      }
    }
    else
    {
      for (;;)
      {
        if (gameplay.is_game_over)
        {
          gameplay.is_game_over = false;
          resetCounter();
          showGameOver();
          break;
        }
        Player player_1 = createPlayer(PLAYER_1_START_XPOS, PLAYER_1_START_YPOS);
        Robot robot = createRobot(PLAYER_2_START_XPOS, PLAYER_2_START_YPOS, DIRECTION_RAND());

        Orb orb = createOrb(ORB_START_XPOS, ORB_START_YPOS, ORB_SPEED_RAND(), ORB_SPEED_RAND(), DIRECTION_RAND(), DIRECTION_RAND());

        initGraphics();
        SSD1306_Display();
        SCR1_Timer_Delay(1500000);
        for (;;)
        {
          if (renderPauseIfNeeded(&orb, &player_1, &robot.coordinates))
          {
              SCR1_Timer_Delay(30000);
              continue;
          }
          SSD1306_ClearDisplay();
          changeOrbPosition(&orb);
          if (changeOrbDirIfCollisions(&orb, &player_1, &robot.coordinates))
          {
            SCR1_Timer_Delay(1500000);
            break;
          }
          changePlayerPosIfButtonPressedAndManageRobot(&player_1, &robot, &orb);

          drawPlatform(&player_1);
          drawPlatform(&robot.coordinates);
          drawOrb(&orb);
          SSD1306_Display();
        }
      }
    }
  }
}

Player createPlayer(int8_t x_pos, int8_t y_pos)
{
  return (Player){ .x = x_pos, .y = y_pos };
}

Robot createRobot(int8_t x_pos, int8_t y_pos, bool direction)
{
  return (Robot){ .coordinates.x = x_pos, .coordinates.y = y_pos, .direction = direction, .aim_offset = 0, .tracking = false };
}

Orb createOrb(int16_t x_pos, int16_t y_pos, int8_t x_speed, int8_t y_speed, bool x_dir, bool y_dir)
{
  return (Orb){ .x = x_pos, .y = y_pos, .x_speed = x_speed, .y_speed = y_speed, .x_direction = x_dir, .y_direction = y_dir, .hits = 0 };
}

void drawOrb(Orb * orb)
{
  SSD1306_FillCircle(orb->x, orb->y, ORB_RADIUS, SSD1306_WHITE);
}

void drawPlatform(Player * player)
{
  SSD1306_FillRect(player->x, player->y, PLATFORM_WIDTH, PLATFORM_HEIGHT, SSD1306_WHITE);
}

void showMenu(void)
{
  SSD1306_ClearDisplay();

  SSD1306_SetTextColor(SSD1306_WHITE);
  SSD1306_SetTextSize(2);
  SSD1306_SetCursor(10, 10);
  SSD1306_WriteText("PING-PONG");
  SSD1306_FillCircle(87, 15, 1, SSD1306_WHITE);
  SSD1306_SetTextSize(1);
  SSD1306_SetCursor(10, 30);
  switch (gameplay.mode)
  {
    case (PLAYER_VS_PLAYER):

      SSD1306_WriteText("> PLAYER VS PLAYER");
      SSD1306_SetCursor(10, 42);
      SSD1306_WriteText("  PLAYER VS ROBOT");
      SSD1306_SetCursor(10, 54);
      SSD1306_WriteText("  SETTINGS");
      break;

    case (PLAYER_VS_ROBOT):

      SSD1306_WriteText("  PLAYER VS PLAYER");
      SSD1306_SetCursor(10, 42);
      SSD1306_WriteText("> PLAYER VS ROBOT");
      SSD1306_SetCursor(10, 54);
      SSD1306_WriteText("  SETTINGS");
      break;

    case (SETTINGS):

      SSD1306_WriteText("  PLAYER VS PLAYER");
      SSD1306_SetCursor(10, 42);
      SSD1306_WriteText("  PLAYER VS ROBOT");
      SSD1306_SetCursor(10, 54);
      SSD1306_WriteText("> SETTINGS");
      break;
  }

  SSD1306_Display();
}

void chooseMode(void)
{
  showMenu();

  while (1)
  {
    playMenuMusic(gameplay.music);
    if (HAL_GPIO_ReadPin(BUTTON_DOWN_PLAYER_1_PORT, 1 << BUTTON_DOWN_PLAYER_1_PIN) == GPIO_PIN_HIGH)
    {
      switch (gameplay.mode)
      {
        case (PLAYER_VS_PLAYER):

          gameplay.mode = PLAYER_VS_ROBOT;
          break;

        case (PLAYER_VS_ROBOT):

          gameplay.mode = SETTINGS;
          break;

        case (SETTINGS):

          gameplay.mode = PLAYER_VS_PLAYER;
          break;
      }

      showMenu();
      SCR1_Timer_Delay(200000);
    }

    if (HAL_GPIO_ReadPin(BUTTON_UP_PLAYER_1_PORT, 1 << BUTTON_UP_PLAYER_1_PIN) == GPIO_PIN_HIGH)
    {
      if (gameplay.mode == SETTINGS)
      {
        noTone();
        settingsMode();
        gameplay.mode = PLAYER_VS_PLAYER;
        showMenu();
        SCR1_Timer_Delay(200000);
      }
      else
      {
        break;
      }
    }
  }

  noTone();
}

void showSettings(void)
{
  // Индексы массивов - значения enum Settings, поэтому порядок строк на экране совпадает с порядком в enum
  static char * const selected_rows[] =
  {
    [MAX_SCORE]  = "> MAX SCORE:",
    [DIFFICULTY] = "> DIFFICULTY:",
    [MUSIC]      = "> MUSIC:",
    [GO_BACK]    = "< GO BACK"
  };
  static char * const unselected_rows[] =
  {
    [MAX_SCORE]  = "  MAX SCORE:",
    [DIFFICULTY] = "  DIFFICULTY:",
    [MUSIC]      = "  MUSIC:",
    [GO_BACK]    = "  GO BACK"
  };

  SSD1306_ClearDisplay();

  SSD1306_SetTextSize(2);
  SSD1306_SetCursor(10, 0);
  SSD1306_WriteText("SETTINGS");
  SSD1306_SetTextSize(1);

  // Четыре строки с шагом 12 пикселей: y = 18, 30, 42, 54
  for (int row = MAX_SCORE; row <= GO_BACK; ++row)
  {
    SSD1306_SetCursor(10, 18 + 12 * row);
    SSD1306_WriteText((row == (int)gameplay.settings) ? selected_rows[row] : unselected_rows[row]);
  }

  SSD1306_SetCursor(100, 18);
  SSD1306_WriteText(itoa(gameplay.max_score, gameplay.counter.score_buf, 10));

  SSD1306_SetCursor(90, 30);
  switch (gameplay.difficulty)
  {
    case (ROBOT_EASY):

      SSD1306_WriteText("EASY");
      break;

    case (ROBOT_NORMAL):

      SSD1306_WriteText("NORMAL");
      break;

    case (ROBOT_HARD):

      SSD1306_WriteText("HARD");
      break;
  }

  SSD1306_SetCursor(90, 42);
  switch (gameplay.music)
  {
    case (RETRO):

      SSD1306_WriteText("RETRO");
      break;

    case (METAL):

      SSD1306_WriteText("METAL");
      break;

    case (OFF):

      SSD1306_WriteText(" OFF");
      break;
  }

  SSD1306_Display();
}

// EASY -> NORMAL -> HARD -> EASY
void nextDifficulty(void)
{
  switch (gameplay.difficulty)
  {
    case (ROBOT_EASY):

      gameplay.difficulty = ROBOT_NORMAL;
      break;

    case (ROBOT_NORMAL):

      gameplay.difficulty = ROBOT_HARD;
      break;

    case (ROBOT_HARD):

      gameplay.difficulty = ROBOT_EASY;
      break;
  }
}

void settingsMode(void)
{
  gameplay.settings = MAX_SCORE;
  showSettings();
  SCR1_Timer_Delay(200000);

  while (1)
  {
    if (HAL_GPIO_ReadPin(BUTTON_DOWN_PLAYER_1_PORT, 1 << BUTTON_DOWN_PLAYER_1_PIN) == GPIO_PIN_HIGH)
    {
      switch (gameplay.settings)
      {
        case (MAX_SCORE):

          gameplay.settings = DIFFICULTY;
          break;

        case (DIFFICULTY):

          gameplay.settings = MUSIC;
          break;

        case (MUSIC):

          gameplay.settings = GO_BACK;
          break;

        case (GO_BACK):

          gameplay.settings = MAX_SCORE;
          break;
      }

      showSettings();
      SCR1_Timer_Delay(200000);
    }

    if (HAL_GPIO_ReadPin(BUTTON_UP_PLAYER_1_PORT, 1 << BUTTON_UP_PLAYER_1_PIN) == GPIO_PIN_HIGH)
    {
      if (gameplay.settings == MAX_SCORE)
      {
        if (gameplay.max_score != 10)
        {
          ++gameplay.max_score;
        }
        else
        {
          gameplay.max_score = 1;
        }
      }
      else if (gameplay.settings == DIFFICULTY)
      {
        nextDifficulty();
      }
      else if (gameplay.settings == MUSIC)
      {
        if (gameplay.music == RETRO)
        {
          gameplay.music = METAL;
        }
        else if (gameplay.music == METAL)
        {
          gameplay.music = OFF;
        }
        else
        {
          gameplay.music = RETRO;
        }
      }
      else
      {
        break;
      }

      showSettings();
      SCR1_Timer_Delay(200000);
    }
  }
}

static bool renderPauseIfNeeded(Orb *orb, Player *player_1, Player *player_2)
{
    bool pause_pressed =
      HAL_GPIO_ReadPin(
        PAUSE_BUTTON_PORT,
        1 << PAUSE_BUTTON_PIN
      ) == GPIO_PIN_HIGH;
    if (pause_pressed && !gameplay.pause_button_was_pressed)
    {
      gameplay.is_paused = !gameplay.is_paused;
    }
    gameplay.pause_button_was_pressed = pause_pressed;
    if (!gameplay.is_paused)
    {
      return false;
    }
    SSD1306_ClearDisplay();
    drawPlatform(player_1);
    drawPlatform(player_2);
    drawOrb(orb);
    SSD1306_FillRect(41, 23, 46, 18, SSD1306_WHITE);
    SSD1306_FillRect(42, 24, 44, 16, SSD1306_BLACK);
    SSD1306_SetTextSize(1);
    SSD1306_SetTextColor(SSD1306_WHITE);
    SSD1306_SetCursor(46, 28);
    SSD1306_WriteText("PAUSED");
    SSD1306_Display();
    return true;
}

void initGraphics(void)
{
  SSD1306_FillRect(PLAYER_1_START_XPOS, PLAYER_1_START_YPOS, PLATFORM_WIDTH, PLATFORM_HEIGHT, SSD1306_WHITE);
  SSD1306_FillRect(PLAYER_2_START_XPOS, PLAYER_2_START_YPOS, PLATFORM_WIDTH, PLATFORM_HEIGHT, SSD1306_WHITE);
  SSD1306_FillCircle(ORB_START_XPOS, ORB_START_YPOS, ORB_RADIUS, SSD1306_WHITE);
  SSD1306_SetTextSize(2);
  SSD1306_SetCursor(32, 5);
  SSD1306_WriteText(itoa(gameplay.counter.player_1_score, gameplay.counter.score_buf, 10));
  SSD1306_SetCursor(88, 5);
  SSD1306_WriteText(itoa(gameplay.counter.player_2_score, gameplay.counter.score_buf, 10));
}

void showGameOver(void)
{
  SSD1306_SetTextSize(2);
  if (gameplay.player_1_won)
  {
    SSD1306_SetCursor(14, 20);
    SSD1306_WriteText("PLAYER 1");
    SSD1306_SetCursor(43, 36);
    SSD1306_WriteText("WON");
  }
  else
  {
    if (gameplay.mode == PLAYER_VS_PLAYER)
    {
      SSD1306_SetCursor(14, 20);
      SSD1306_WriteText("PLAYER 2");
      SSD1306_SetCursor(43, 36);
      SSD1306_WriteText("WON");
    }
    else
    {
      SSD1306_SetCursor(12, 20);
      SSD1306_WriteText("  ROBOT");
      SSD1306_SetCursor(48, 36);
      SSD1306_WriteText("WON");
    }
  }
  SSD1306_Display();
  SCR1_Timer_Delay(2000000);
}

void incScorePlayer_1()
{
  if (++gameplay.counter.player_1_score == gameplay.max_score)
  {
    gameplay.is_game_over = true;
    gameplay.player_1_won = true;
  }
}

void incScorePlayer_2()
{
  if (++gameplay.counter.player_2_score == gameplay.max_score)
  {
    gameplay.is_game_over = true;
    gameplay.player_1_won = false;
  }
}

void resetCounter()
{
  gameplay.counter.player_1_score = 0;
  gameplay.counter.player_2_score = 0;
}

void changeOrbPosition(Orb * orb)
{
  if (orb->x_direction)
  {
    orb->x += orb->x_speed;
  }
  else
  {
    orb->x -= orb->x_speed;
  }

  if (orb->y_direction)
  {
    orb->y += orb->y_speed;
  }
  else
  {
    orb->y -= orb->y_speed;
  }
}

bool changeOrbDirIfCollisions(Orb * orb, Player * player_1, Player * player_2)
{
  if (orb->x <= 0)
  {
    incScorePlayer_2();
    return 1;
  }
  else if (orb->x >= (SSD1306_WIDTH - ORB_RADIUS))
  {
    incScorePlayer_1();
    return 1;
  }

  if (orb->y <= 0)
  {
    doCollisionSound();
    orb->y_direction = true;
    orb->y_speed = ORB_SPEED_RAND();
  }
  else if (orb->y >= (SSD1306_HEIGHT - ORB_RADIUS))
  {
    doCollisionSound();
    orb->y_direction = false;
    orb->y_speed = ORB_SPEED_RAND();
  }

  // Платформа учитывается, только если шарик летит К ней. Иначе, пока шарик ещё
  // внутри зоны отбития, один удар засчитывался бы несколько раз подряд.
  if ( !orb->x_direction && (orb->y <= (player_1->y + PLATFORM_HEIGHT)) && (orb->y >= player_1->y) && (orb->x <= (player_1->x + PLATFORM_WIDTH + ORB_RADIUS)) )
  {
    doCollisionSound();
    bounceOrbFromPlatform(orb, true);
  }
  else if ( orb->x_direction && (orb->y <= (player_2->y + PLATFORM_HEIGHT)) && (orb->y >= player_2->y) && (orb->x >= (player_2->x - ORB_RADIUS)) )
  {
    doCollisionSound();
    bounceOrbFromPlatform(orb, false);
  }
  return 0;
}

// Отскок от платформы: меняет направление по X и пересчитывает скорость с учётом ускорения
void bounceOrbFromPlatform(Orb * orb, bool new_x_direction)
{
  orb->x_direction = new_x_direction;

  if (orb->hits < UINT8_MAX)
  {
    ++orb->hits;
  }

  int16_t speed = ORB_SPEED_RAND() + (orb->hits / ORB_HITS_PER_SPEEDUP);
  orb->x_speed = (speed > ORB_SPEED_CAP) ? ORB_SPEED_CAP : speed;
}

void changePlayersPosIfButtonsPressed(Player * player_1, Player * player_2)
{
  if (player_1->y > 0 && HAL_GPIO_ReadPin(BUTTON_DOWN_PLAYER_1_PORT, 1 << BUTTON_DOWN_PLAYER_1_PIN) == GPIO_PIN_HIGH)
  {
    player_1->y -= PLAYER_SPEED;
  }

  if (player_1->y < (SSD1306_HEIGHT - PLATFORM_HEIGHT) && HAL_GPIO_ReadPin(BUTTON_UP_PLAYER_1_PORT, 1 << BUTTON_UP_PLAYER_1_PIN) == GPIO_PIN_HIGH)
  {
    player_1->y += PLAYER_SPEED;
  }

  if (player_2->y > 0 && HAL_GPIO_ReadPin(BUTTON_DOWN_PLAYER_2_PORT, 1 << BUTTON_DOWN_PLAYER_2_PIN) == GPIO_PIN_HIGH)
  {
    player_2->y -= PLAYER_SPEED;
  }

  if (player_2->y < (SSD1306_HEIGHT - PLATFORM_HEIGHT) && HAL_GPIO_ReadPin(BUTTON_UP_PLAYER_2_PORT, 1 << BUTTON_UP_PLAYER_2_PIN) == GPIO_PIN_HIGH)
  {
    player_2->y += PLAYER_SPEED;
  }
}

// ROBOT_EASY: старый бот. Ездит вверх-вниз, отскакивая от границ экрана, и не смотрит на шарик
void moveRobotZigzag(Robot * robot)
{
  if (!robot->direction)
  {
    if (robot->coordinates.y > 0)
    {
      robot->coordinates.y -= PLAYER_SPEED;
    }
    else
    {
      robot->direction = true;
    }
  }
  else if (robot->direction)
  {
    if (robot->coordinates.y < (SSD1306_HEIGHT - PLATFORM_HEIGHT))
    {
      robot->coordinates.y += PLAYER_SPEED;
    }
    else
    {
      robot->direction = false;
    }
  }
}

// ROBOT_NORMAL и ROBOT_HARD: шарик летит к боту - едет за шариком с ошибкой прицеливания
// (до aim_error_max пикселей), не к нему - возвращается в центр
void moveRobotTracking(Robot * robot, const Orb * orb, int8_t aim_error_max)
{
  int16_t target_y;

  if (orb->x_direction)
  {
    if (!robot->tracking)
    {
      // Новый подлёт: выбираем ошибку один раз, а не каждый кадр, иначе бот "дрожал" бы
      robot->tracking = true;
      robot->aim_offset = (rand() % (2 * aim_error_max + 1)) - aim_error_max;
    }
    target_y = orb->y + robot->aim_offset - (PLATFORM_HEIGHT / 2);
  }
  else
  {
    robot->tracking = false;
    target_y = (SSD1306_HEIGHT - PLATFORM_HEIGHT) / 2;
  }

  if (target_y < 0)
  {
    target_y = 0;
  }
  else if (target_y > (SSD1306_HEIGHT - PLATFORM_HEIGHT))
  {
    target_y = SSD1306_HEIGHT - PLATFORM_HEIGHT;
  }

  int16_t diff = target_y - robot->coordinates.y;
  int16_t distance = (diff < 0) ? -diff : diff;
  if (distance < ROBOT_DEAD_ZONE)
  {
    return;
  }

  // Шаг не больше PLAYER_SPEED и не больше расстояния до цели (чтобы не проскочить её)
  int16_t step = (distance < PLAYER_SPEED) ? distance : PLAYER_SPEED;
  robot->coordinates.y += (diff > 0) ? step : -step;
}

// Выбор поведения бота по выбранной в настройках сложности
void moveRobot(Robot * robot, const Orb * orb)
{
  switch (gameplay.difficulty)
  {
    case (ROBOT_EASY):

      moveRobotZigzag(robot);
      break;

    case (ROBOT_NORMAL):

      moveRobotTracking(robot, orb, ROBOT_NORMAL_AIM_ERROR_MAX);
      break;

    case (ROBOT_HARD):

      moveRobotTracking(robot, orb, ROBOT_HARD_AIM_ERROR_MAX);
      break;
  }
}

void changePlayerPosIfButtonPressedAndManageRobot(Player * player, Robot * robot, const Orb * orb)
{
  if (player->y > 0 && HAL_GPIO_ReadPin(BUTTON_DOWN_PLAYER_1_PORT, 1 << BUTTON_DOWN_PLAYER_1_PIN) == GPIO_PIN_HIGH)
  {
    player->y -= PLAYER_SPEED;
  }

  if (player->y < (SSD1306_HEIGHT - PLATFORM_HEIGHT) && HAL_GPIO_ReadPin(BUTTON_UP_PLAYER_1_PORT, 1 << BUTTON_UP_PLAYER_1_PIN) == GPIO_PIN_HIGH)
  {
    player->y += PLAYER_SPEED;
  }

  moveRobot(robot, orb);
}
