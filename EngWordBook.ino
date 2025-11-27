#include <SPI.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
#include "esp_sleep.h"

TFT_eSPI tft = TFT_eSPI();

// ---- 핀 설정 (TTGO T-Display 기준, 보드에 따라 다르면 수정) ----
#define BTN_PREV 35      // 위 버튼 (GPIO 35, 입력 전용)
#define BTN_NEXT 0       // 아래 버튼 (GPIO 0, BOOT 버튼)
#define TFT_BL   4       // 백라이트 핀 (보통 GPIO 4)

// ---- 단어 리스트 ----
const int MAX_WORDS = 3000;
String engWords[MAX_WORDS];
int wordCount = 0;
int currentIndex = 0;

int order[MAX_WORDS];   // 단어 표시 순서를 저장할 배열


// ---- 상태 ----
enum AppState {
  STATE_SPLASH,
  STATE_VOCAB
};

AppState appState = STATE_SPLASH;

// 버튼 상태 저장 (엣지 감지용)
int prevPrevBtnState = HIGH;
int prevNextBtnState = HIGH;

// 마지막 입력 시간 (딥슬립용)
unsigned long lastInteraction = 0;
const unsigned long IDLE_SLEEP_MS = 60000UL;  // 1분

// ---------------------------------------------------
// 문자열 앞뒤의 쌍따옴표(") 제거
// ---------------------------------------------------
void stripQuotes(String &s) {
  s.trim();
  if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) {
    s = s.substring(1, s.length() - 1);
  }
}

// ---------------------------------------------------
// LittleFS에서 words.csv 읽기
//  - 1열: 영어단어만 사용
//  - 2열(뜻)은 무시
//  - "apple","사과" 형식도 처리
// ---------------------------------------------------
void loadWordsFromFS() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  fs::File f = LittleFS.open("/words.txt", "r");
  if (!f) {
    Serial.println("Cannot open /words.txt");
    return;
  }

  wordCount = 0;

  while (f.available() && wordCount < MAX_WORDS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // UTF-8 BOM 제거 (첫 줄일 경우)
    if (line.length() >= 3 &&
        (uint8_t)line[0] == 0xEF &&
        (uint8_t)line[1] == 0xBB &&
        (uint8_t)line[2] == 0xBF) {
      line.remove(0, 3);
    }

    int commaIndex = line.indexOf(',');

    String eng;
    if (commaIndex < 0) {
      // 콤마가 없으면 전체를 단어로 사용
      eng = line;
    } else {
      // 첫 번째 열만 사용
      eng = line.substring(0, commaIndex);
    }

    stripQuotes(eng);
    eng.trim();
    if (eng.length() == 0) continue;

    engWords[wordCount] = eng;
    wordCount++;
  }

  f.close();
  Serial.printf("Loaded words: %d\n", wordCount);

  // 혹시 비어 있으면 예비 단어 몇 개 넣기
  if (wordCount == 0) {
    engWords[0] = "apple";
    engWords[1] = "book";
    engWords[2] = "happy";
    wordCount = 3;
  }
}

// ---------------------------------------------------
// 스플래시 화면 (그래픽 + 영어 텍스트만)
// ---------------------------------------------------
void drawSplash() {
  tft.fillScreen(TFT_BLACK);

  // 배경 장식: 색색 원
  tft.fillCircle(20, 20, 15, TFT_BLUE);
  tft.fillCircle(220, 30, 20, TFT_RED);
  tft.fillCircle(200, 120, 18, TFT_GREEN);
  tft.fillCircle(40, 100, 10, TFT_YELLOW);

  // 책 모양 박스
  int bx = 60;
  int by = 30;
  int bw = 120;
  int bh = 80;
  tft.fillRoundRect(bx, by, bw, bh, 10, TFT_NAVY);
  tft.drawRoundRect(bx, by, bw, bh, 10, TFT_WHITE);
  tft.drawLine(bx + bw / 2, by, bx + bw / 2, by + bh, TFT_WHITE); // 중앙 접힘선

  // "BOOK" 아이콘 느낌으로 줄무늬
  tft.drawLine(bx + 10, by + 20, bx + bw - 10, by + 20, TFT_DARKGREY);
  tft.drawLine(bx + 10, by + 35, bx + bw - 10, by + 35, TFT_DARKGREY);
  tft.drawLine(bx + 10, by + 50, bx + bw - 10, by + 50, TFT_DARKGREY);

  // 제목 텍스트 (영어만 사용)
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.setTextSize(2);
  tft.drawString("ENGLISH", 120, 50);
  tft.drawString("WORD BOOK", 120, 75);

  // 아래 안내 문구 (영어)
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Press any button to start", 120, 120);
}

// 단어가 너무 길면 두 줄로 나눠서 가운데 정렬로 그리기
void drawWordWrapped(const String &word, int centerY) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(3);  // 여기 크기는 기존과 동일하게

  int maxWidth = tft.width() - 20; // 좌우 10px 여유

  // 1줄에 다 들어가면 그냥 그리기
  if (tft.textWidth(word) <= maxWidth) {
    tft.drawString(word, tft.width() / 2, centerY);
    return;
  }

  // 안 들어가면 글자를 앞에서부터 차례대로 넣다가 넘치면 2번째 줄로
  String line1 = "";
  String line2 = "";

  for (int i = 0; i < word.length(); i++) {
    String test = line1 + word[i];
    if (tft.textWidth(test) <= maxWidth || line1.length() == 0) {
      line1 = test;       // 아직 첫 줄에 들어감
    } else {
      line2 += word[i];   // 넘치면 두 번째 줄로
    }
  }

  if (line2.length() == 0) {
    // 혹시라도 다 1줄에 들어갔으면 그냥 중앙에
    tft.drawString(line1, tft.width() / 2, centerY);
  } else {
    // 2줄로 나눠서 위·아래로 배치
    tft.drawString(line1, tft.width() / 2, centerY - 10);
    tft.drawString(line2, tft.width() / 2, centerY + 15);
  }
}

// ---------------------------------------------------
// 현재 단어 화면
// ---------------------------------------------------
void drawCurrentWord() {
  if (wordCount == 0) return;

  tft.fillScreen(TFT_BLACK);

  // 상단 인덱스
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  String header = String(currentIndex + 1) + " / " + String(wordCount);
  tft.drawString(header, 5, 5);

  // 가운데 영어 단어 크게
  //tft.setTextDatum(MC_DATUM);
  //tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  //tft.setTextSize(3);  // 단어 길이에 따라 필요하면 조절
  //tft.drawString(engWords[currentIndex], 120, 60);

  // 가운데 영어 단어 (길면 두 줄로)
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  drawWordWrapped(engWords[order[currentIndex]], 60);

  // 하단 버튼 안내 (영어)
  tft.setTextDatum(BC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("UP: prev   DOWN: next", 120, 130);
}

void shuffleWords() {
  // order 배열을 0 ~ wordCount-1로 초기화
  for (int i = 0; i < wordCount; i++) {
    order[i] = i;
  }

  // Fisher–Yates shuffle
  for (int i = wordCount - 1; i > 0; i--) {
    int j = random(0, i + 1);
    int temp = order[i];
    order[i] = order[j];
    order[j] = temp;
  }
}


// ---------------------------------------------------
// 딥슬립 진입
// ---------------------------------------------------
void enterDeepSleep() {
  // LCD 끄기
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
  delay(100);

  // 백라이트 끄기
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // GPIO0(아래 버튼)을 LOW로 눌렀을 때 깨우기
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  Serial.println("Entering deep sleep...");
  delay(100);
  esp_deep_sleep_start();
}

// ---------------------------------------------------
// 버튼 입력 (눌림 순간 감지)
// ---------------------------------------------------
void readButtons(bool &prevPressed, bool &nextPressed) {
  int prevState = digitalRead(BTN_PREV);
  int nextState = digitalRead(BTN_NEXT);

  prevPressed = (prevPrevBtnState == HIGH && prevState == LOW);
  nextPressed = (prevNextBtnState == HIGH && nextState == LOW);

  prevPrevBtnState = prevState;
  prevNextBtnState = nextState;
}


// ---------------------------------------------------
// setup
// ---------------------------------------------------
void setup() {
  randomSeed(esp_random());

  Serial.begin(115200);

  pinMode(BTN_PREV, INPUT);        // GPIO35: 입력 전용
  pinMode(BTN_NEXT, INPUT_PULLUP); // GPIO0: 풀업

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);      // 백라이트 ON

  tft.init();
  tft.setRotation(1);              // 가로 방향

  loadWordsFromFS();
  shuffleWords();    // ★ 단어 순서 랜덤화

  appState = STATE_SPLASH;
  drawSplash();
  lastInteraction = millis();
}

// ---------------------------------------------------
// loop
// ---------------------------------------------------
void loop() {
  bool prevPressed = false;
  bool nextPressed = false;

  readButtons(prevPressed, nextPressed);
  unsigned long now = millis();

  if (prevPressed || nextPressed) {
    lastInteraction = now;
  }

    // ✅ 상태와 상관없이 1분 무입력이면 딥슬립
  if (now - lastInteraction > IDLE_SLEEP_MS) {
    enterDeepSleep();
  }

  switch (appState) {
    case STATE_SPLASH:
      if (prevPressed || nextPressed) {
        appState = STATE_VOCAB;
        currentIndex = 0;
        drawCurrentWord();
      }
      break;

    case STATE_VOCAB:
      if (prevPressed) {
        if (wordCount > 0) {
          currentIndex = (currentIndex - 1 + wordCount) % wordCount;
          drawCurrentWord();
        }
      }
      if (nextPressed) {
        if (wordCount > 0) {
          currentIndex = (currentIndex + 1) % wordCount;
          drawCurrentWord();
        }
      }

      //if (now - lastInteraction > IDLE_SLEEP_MS) {
      //  enterDeepSleep();
      //}
      break;
  }

  delay(20); // 간단한 디바운스
}
