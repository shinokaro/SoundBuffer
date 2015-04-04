/*
 * Copyright (c) <2015> <shinokaro>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *    1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 *
 *    2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 *
 *    3. This notice may not be removed or altered from any source
 *    distribution.
 */
/*
 * このコードはmirichi氏の書いたコードを元にshinokaroが作成した。
 * RubyからDirectSoundの利用方を示してくれたmirichi氏に感謝する。
 */
#include "ruby.h"
#include "ruby/thread.h"
#include <string.h>
/*
 * DirectSoundではGUIDを引数に使用することがある。
 * GUID_NULLの定義が必要になる。このためks.hファイルとlibuuidをリンクする必要がある。
 */
#include "ks.h"
/*
 * MSから配布されている開発キットをインストールしてdsound.hとsal.hを入手する必要がある。
 * MinGW付属のdsound.hはDirectXで使用できる全ての機能をサポートしていない。
 * また、MinGWが提供するdsound.hはWineプロジェクトのものであり、そのライセンスはGPLである。
 *
 * MSのsdound.hはsal.hを内部でインクルードしている。
 * このままコンパイルすると__nullが定義されないためエラーが出る。
 * そのため__nullを自前で定義する必要がある。
 */
#define __null
#define DIRECTSOUND_VERSION 0x0900
#include <dsound.h>

// Ruby側のエフェクト指定用の定数
// クラス定義のセクションでRuby定数定義を行っている。
#define FX_GARGLE       0
#define FX_CHORUS       1
#define FX_FLANGER      2
#define FX_ECHO         3
#define FX_DISTORTION   4
#define FX_COMPRESSOR   5
#define FX_PARAM_EQ     6
#define FX_I3DL2_REVERB 7
#define FX_WAVES_REVERB 8

// 通知イベントの固定ハンドル数
#define EVENT_PRESET    3

// RubyのSoundTestクラス
static VALUE cSoundBuffer;

// Rubyの例外オブジェクト
static VALUE eSoundBufferError;

// COMのDirectSoundオブジェクト
static LPDIRECTSOUND8       g_pDSound;
static LPDIRECTSOUNDBUFFER  g_pDSBuffer;

// RubyのSoundTestオブジェクトが持つC構造体
struct SoundBuffer {
  LPDIRECTSOUNDBUFFER8  pDSBuffer8;
  VALUE                 origin;
  DWORD                 copy_flag;
  size_t                buffer_bytes;
  WORD                  channels;
  DWORD                 samples_per_sec;
  WORD                  bits_per_sample;
  WORD                  block_align;
  DWORD                 avg_bytes_per_sec;
  DWORD                 effect_flag;
  DWORD                 effect_count;
  LPDWORD               effect_nums;
  DWORD                 play_flag;
  DWORD                 repeat_flag;
  DWORD                 loop_flag;
  DWORD                 loop_start;
  DWORD                 loop_end;
  DWORD                 loop_count;
  DWORD                 loop_counter;
  DWORD                 event_count;
  LPDWORD               event_offsets;
  LPHANDLE              event_handles;
  HANDLE                event_loop_point;
  HANDLE                event_offsetstop;
  HANDLE                event_wait_break;
};

// notify_wait_blockingの引数に与えるための型データ
struct NotifyData {
  DWORD     result;
  DWORD     timeout;
  struct SoundBuffer* st;
};

// プロトタイプ宣言
static void   SoundBuffer_mark(void*);
static void   SoundBuffer_free(void*);
static size_t SoundBuffer_memsize(const void*);
static VALUE  SoundBuffer_stop(VALUE);
static VALUE  SoundBuffer_set_notify(int, VALUE*, VALUE);
static struct SoundBuffer* get_st(VALUE);
static void   clear_st_event(struct SoundBuffer*);
static void   to_raise_an_exception(HRESULT);
static DWORD  get_playing(struct SoundBuffer*);
static void   stop_sound(struct SoundBuffer*);
static DWORD  row2pcm(struct SoundBuffer*, DWORD);
static VALUE  row2pcmnum(struct SoundBuffer*, DWORD);
static DWORD  pcmnum2row(struct SoundBuffer*, VALUE);
static VALUE  SoundBuffer_set_notify(int, VALUE*, VALUE);
static void   create_st_event(struct SoundBuffer*, DWORD, LPDWORD);
// TypedData用の型データ
const rb_data_type_t SoundBuffer_data_type = {
  "SoundBuffer",
  {
    SoundBuffer_mark,
    SoundBuffer_free,
    SoundBuffer_memsize,
  },
  NULL,
  NULL
};

// COM終了条件はshutdown+すべてのSoundBufferの解放なのでカウントする
static int g_refcount = 0;

/*
 *
 */
static void
SoundBuffer_mark(void *s)
{
  struct SoundBuffer *st = (struct SoundBuffer *)s;

  rb_gc_mark(st->origin);
}

static void
clear_st_event(struct SoundBuffer *st)
{
  DWORD i;

  if (st->event_handles) {
    for (i = 0; i < st->event_count; i++) {
      if (!(st->event_handles[i] == st->event_loop_point
         || st->event_handles[i] == st->event_offsetstop
         || st->event_handles[i] == st->event_wait_break))
      CloseHandle(st->event_handles[i]);
    }
    xfree(st->event_handles);
    st->event_handles = NULL;
  }
  if (st->event_offsets) {
    xfree(st->event_offsets);
    st->event_offsets = NULL;
  }
  st->event_count = 0;
}

static void
create_st_event_presets(struct SoundBuffer *st)
{
  if (!st->event_loop_point) {
    st->event_loop_point = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!st->event_loop_point) rb_raise(eSoundBufferError, "create_st_event_presets error");
  }
  if (!st->event_offsetstop) {
    st->event_offsetstop = CreateEvent(NULL, TRUE,  FALSE, NULL);
    if (!st->event_offsetstop) rb_raise(eSoundBufferError, "create_st_event_presets error");
  }
  if (!st->event_wait_break) {
    st->event_wait_break = CreateEvent(NULL, TRUE,  FALSE, NULL);
    if (!st->event_wait_break) rb_raise(eSoundBufferError, "create_st_event_presets error");
  }
}

static void
clear_st_event_presets(struct SoundBuffer *st)
{
  if (st->event_loop_point) {
    CloseHandle(st->event_loop_point);
    st->event_loop_point = NULL;
  }
  if (st->event_offsetstop) {
    CloseHandle(st->event_offsetstop);
    st->event_offsetstop = NULL;
  }
  if (st->event_wait_break) {
    CloseHandle(st->event_wait_break);
    st->event_wait_break = NULL;
  }
}

static void
clear_st_effect(struct SoundBuffer *st)
{
  if (st->effect_nums) {
    xfree(st->effect_nums);
    st->effect_nums = NULL;
  }
  st->effect_count = 0;
}

// DirectSoundバッファを開放する内部用関数
static void
SoundBuffer_release(struct SoundBuffer *st)
{
  if (st->pDSBuffer8) {
    st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
    st->pDSBuffer8->lpVtbl->Release(st->pDSBuffer8);
    st->pDSBuffer8    = NULL;
    st->buffer_bytes  = 0;
    st->origin        = Qnil;
    clear_st_effect(st);
    clear_st_event(st);
    clear_st_event_presets(st);

    // shutduwn+すべてのSoundTestが解放されたらDirectSound解放
    g_refcount--;
    if (g_refcount == 0) {
      g_pDSound->lpVtbl->Release(g_pDSound);
      CoUninitialize();
    }
  }
}

static void
SoundBuffer_free(void *s)
{
  struct SoundBuffer *st = (struct SoundBuffer *)s;
  // バッファ解放
  SoundBuffer_release(st);
  // SoundTest解放
  xfree(st);
}

static size_t
SoundBuffer_memsize(const void *s)
{
  struct SoundBuffer *st = (struct SoundBuffer *)s;

  return sizeof(struct SoundBuffer)
       + (st->copy_flag ? 0 : st->buffer_bytes)
       + st->effect_count * sizeof(DWORD)
       + st->event_count  * (sizeof(HANDLE) + sizeof(DWORD));
}

static struct SoundBuffer *
get_st(VALUE self)
{
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);
  if (!st->pDSBuffer8) rb_raise(eSoundBufferError, "disposed object");
  return st;
}

static VALUE
SoundBuffer_allocate(VALUE klass)
{
  VALUE obj;
  struct SoundBuffer *st;

  // RubyのTypedData型オブジェクトを生成する
  obj = TypedData_Make_Struct(klass, struct SoundBuffer, &SoundBuffer_data_type, st);

  // allocate時点ではバッファサイズが不明なのでバッファは作らない
  st->pDSBuffer8        = NULL;
  st->origin            = obj;
  st->copy_flag         = 0;
  st->buffer_bytes      = 0;
  st->channels          = 0;
  st->samples_per_sec   = 0;
  st->bits_per_sample   = 0;
  st->block_align       = 0;
  st->avg_bytes_per_sec = 0;
  st->effect_flag       = 0;
  st->effect_count      = 0;
  st->effect_nums       = NULL;
  st->play_flag         = 0;
  st->repeat_flag       = 0;
  st->loop_flag         = 0;
  st->loop_start        = 0;
  st->loop_end          = 0;
  st->loop_count        = 0;
  st->loop_counter      = 0;
  st->event_count       = 0;
  st->event_handles     = NULL;
  st->event_loop_point  = NULL;
  st->event_offsets     = NULL;
  st->event_wait_break  = NULL;
  return obj;
}

/*
 *
 */
static VALUE
SoundBuffer_initialize_copy(VALUE dst, VALUE src)
{
  DWORD   argc;
  LPDWORD offsets;
  HRESULT hr;
  struct SoundBuffer *src_st, *dst_st;

  rb_call_super(1, &src); // If an exception occurs , src object can not be copied object.
  src_st = get_st(src);   // If an exception occurs , src object is no DSBuffer.
  dst_st = (struct SoundBuffer *)RTYPEDDATA_DATA(dst);
  /*
   *    struct SoundBuffer members & object status
   *    +-----------+-------+-----------------+-----+-----+
   *    |pDSBuffer8 |origin |status           |dst? |src? |
   *    +-----------+-------+-----------------+-----+-----+
   *    |ptr        |self   |origin object    | NG  | OK  |
   *    |ptr        |origin |copied object    | NG  | OK  |
   *    |NULL       |self   |allocated object | OK  | NG  |
   *    |NULL       |Qnil   |disposed object  | NG  | NG  |
   *    +-----------+-------+-----------------+-----+-----+
   */
  if (dst_st->pDSBuffer8 == NULL && dst_st->origin == dst && src_st->pDSBuffer8) {
    hr = g_pDSound->lpVtbl->DuplicateSoundBuffer(g_pDSound, (LPDIRECTSOUNDBUFFER)src_st->pDSBuffer8, (LPDIRECTSOUNDBUFFER *)&dst_st->pDSBuffer8);
    if (FAILED(hr)) to_raise_an_exception(hr);
    g_refcount++;
    // object state members
    dst_st->origin            = src_st->origin;
    dst_st->copy_flag         = 1;
    // DSBuffer parameter members
    dst_st->buffer_bytes      = src_st->buffer_bytes;
    dst_st->channels          = src_st->channels;
    dst_st->samples_per_sec   = src_st->samples_per_sec;
    dst_st->bits_per_sample   = src_st->bits_per_sample;
    dst_st->block_align       = src_st->block_align;
    dst_st->avg_bytes_per_sec = src_st->avg_bytes_per_sec;
    // loop members
    dst_st->loop_flag         = src_st->loop_flag;
    dst_st->loop_start        = src_st->loop_start;
    dst_st->loop_end          = src_st->loop_end;
    dst_st->loop_count        = src_st->loop_count;
    dst_st->loop_counter      = 0;
    // event members
    // new notify handles setup from src_st->event_offsets
    create_st_event_presets(dst_st);
    argc = src_st->event_count - EVENT_PRESET;
    offsets = ALLOCA_N(DWORD, argc);
    MEMCPY(offsets, src_st->event_offsets, DWORD, argc);
    create_st_event(dst_st, argc, offsets);
  }
  else rb_raise(rb_eTypeError, "initialize_copy should take allocated object");

  return dst;
}

static VALUE
SoundBuffer_get_origin(VALUE self)
{
  return get_st(self)->origin == self ? Qtrue : Qfalse;
}

static LPVOID
DSBcpy(LPVOID ptrA, DWORD lenA, LPVOID ptrB, DWORD lenB, char* ptrS, DWORD lenS, LPDWORD wlenA, LPDWORD wlenB)
{
  LPVOID result;
  DWORD writeA, writeB, lenRest;

  writeA = lenS < lenA ? lenS : lenA;
  result = memcpy(ptrA, ptrS, writeA);
  lenRest = lenS - writeA;
  if (wlenA != NULL) *wlenA = writeA;
  if (wlenB != NULL) *wlenB = 0;
  if (ptrB  == NULL || lenB == 0 || lenRest == 0) return result;
  writeB = lenRest < lenB ? lenRest : lenB;
  result = memcpy(ptrB, ptrS + writeA, writeB);
  if (wlenB != NULL) *wlenB = writeB;
  return result;
}

/*
 * call-seq:
 *    sb.write(str) ->  fixnum
 *    sb.write(str, offset) ->  fixnum
 */
static VALUE
SoundBuffer_write(int argc, VALUE *argv, VALUE self)
{
  LPVOID   ptr1,  ptr2;
  DWORD    size1, size2;
  DWORD    bytes, offset, write_size1 = 0, write_size2 = 0, loopying = FALSE, from_write_cursor = FALSE;
  char    *strptr;
  HRESULT  hr;
  VALUE    vbuffer, voffset, vopt;
  struct SoundBuffer *st = get_st(self);

// taint check & reflect

  rb_scan_args(argc, argv, "11:", &vbuffer, &voffset, &vopt);
  // arg1 buffer
  Check_Type(vbuffer, T_STRING);
  bytes  = RSTRING_LEN(vbuffer);
  strptr = RSTRING_PTR(vbuffer);
  if (bytes  > st->buffer_bytes) rb_raise(rb_eRangeError, "string length");
  // arg2 offset
  offset = NIL_P(voffset) ? 0 : NUM2UINT(voffset);
  if (offset > st->buffer_bytes) rb_raise(rb_eRangeError, "offset");
  // opt loopying, from_write_cursor
  if (!NIL_P(vopt)) {
    loopying          = RTEST(rb_hash_aref(vopt, ID2SYM(rb_intern("loopying"))))          ? TRUE : FALSE;
    from_write_cursor = RTEST(rb_hash_aref(vopt, ID2SYM(rb_intern("from_write_cursor")))) ? TRUE : FALSE;
  }
  if (!loopying && offset + bytes > st->buffer_bytes) rb_raise(rb_eRangeError, "this method is nolap mode");
  // buffer write
  if (bytes) {
    hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, offset, bytes, &ptr1, &size1, &ptr2, &size2,
                                      from_write_cursor ? DSBLOCK_FROMWRITECURSOR : DSBLOCK_ENTIREBUFFER);
    if (FAILED(hr)) to_raise_an_exception(hr);

    if (loopying) DSBcpy(ptr1, size1, ptr2, size2, strptr, bytes, &write_size1, &write_size2);
    else          DSBcpy(ptr1, size1, NULL,     0, strptr, bytes, &write_size1, NULL);

    if (RTEST(rb_obj_tainted(vbuffer))) rb_obj_taint(self);

    hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, write_size1, ptr2, 0);
    if (FAILED(hr)) rb_raise(eSoundBufferError, "Unlock error");
  }
  return UINT2NUM(write_size1 + write_size2);
}

/*
 * SoundTest#initialize
 */
static VALUE
SoundBuffer_initialize(int argc, VALUE *argv, VALUE self)
{
  DSBUFFERDESC          desc;
  WAVEFORMATEX          pcmwf;
  LPDIRECTSOUNDBUFFER   pDSBuffer;
  HRESULT hr;
  VALUE   vbuffer, vsamples_per_sec, vbits_per_sample, vchannels, vopt;
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);

  if (st->pDSBuffer8) rb_raise(eSoundBufferError, "object is already initialized");

  rb_scan_args(argc, argv, "13:", &vbuffer, &vchannels, &vsamples_per_sec, &vbits_per_sample, &vopt);
  switch (TYPE(vbuffer)) {
  case T_FIXNUM:
    st->buffer_bytes = NUM2UINT(vbuffer);
    break;
  case T_STRING:
    st->buffer_bytes = RSTRING_LEN(vbuffer);
    break;
  default:
    rb_raise(rb_eTypeError, "not valid value");
    break;
  }
  if (st->buffer_bytes < DSBSIZE_MIN || DSBSIZE_MAX < st->buffer_bytes) rb_raise(rb_eRangeError, "buffer size error");

  st->channels        = NIL_P(vchannels)        ? 1     : (WORD)NUM2UINT(vchannels);
  if (st->channels != 1 && st->channels != 2) rb_raise(rb_eRangeError, "channels arguments 1 and 2 only possible");

  st->samples_per_sec = NIL_P(vsamples_per_sec) ? 48000 :       NUM2UINT(vsamples_per_sec);
  if (st->samples_per_sec < DSBFREQUENCY_MIN || DSBFREQUENCY_MAX < st->samples_per_sec) rb_raise(rb_eRangeError, "samples_per_sec argument can be only DSBFREQUENCY_MIN-DSBFREQUENCY_MAX");

  st->bits_per_sample = NIL_P(vbits_per_sample) ? 16    : (WORD)NUM2UINT(vbits_per_sample);
  if (st->bits_per_sample != 8 && st->bits_per_sample != 16) rb_raise(rb_eRangeError, "bits_per_sample arguments 8 and 16 only possible");

  st->effect_flag = !NIL_P(vopt) && RTEST(rb_hash_aref(vopt, ID2SYM(rb_intern("effect")))) ? 1 : 0;
  if (st->effect_flag && st->bits_per_sample == 8) rb_raise(rb_eRangeError, "do not use FX, when bits per sample is 8 bit");
  // 切捨て判定でOKか、あとで調べる。
  if (st->effect_flag && st->buffer_bytes < st->samples_per_sec * DSBSIZE_FX_MIN / 1000) rb_raise(rb_eRangeError, "buffer is small, when use FX");

  st->block_align       = st->channels * st->bits_per_sample / 8;
  st->avg_bytes_per_sec = st->samples_per_sec * st->block_align;
  // フォーマット設定
  pcmwf.wFormatTag      = WAVE_FORMAT_PCM;
  pcmwf.nChannels       = st->channels;
  pcmwf.nSamplesPerSec  = st->samples_per_sec;
  pcmwf.nAvgBytesPerSec = st->avg_bytes_per_sec;
  pcmwf.nBlockAlign     = st->block_align;
  pcmwf.wBitsPerSample  = st->bits_per_sample;
  pcmwf.cbSize          = 0;
  // DirectSoundバッファ設定
  desc.dwSize           = sizeof(desc);
  desc.dwFlags          = DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | (st->effect_flag ? DSBCAPS_CTRLFX : 0)
                        | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
  desc.dwBufferBytes    = st->buffer_bytes;
  desc.dwReserved       = 0;
  desc.lpwfxFormat      = &pcmwf;
  desc.guid3DAlgorithm  = DS3DALG_DEFAULT;

  // DirectSoundバッファ生成
  hr = g_pDSound->lpVtbl->CreateSoundBuffer(g_pDSound, &desc, &pDSBuffer, NULL);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "CreateSoundBuffer error");
  hr = pDSBuffer->lpVtbl->QueryInterface(pDSBuffer, &IID_IDirectSoundBuffer8, (void**)&(st->pDSBuffer8));
  if (FAILED(hr)) rb_raise(eSoundBufferError, "QueryInterface error");
  pDSBuffer->lpVtbl->Release(pDSBuffer);

  g_refcount++;

  if (TYPE(vbuffer) == T_STRING) SoundBuffer_write(1, &vbuffer, self);
  create_st_event_presets(st);
  SoundBuffer_set_notify(0, NULL, self);

  return self;
}

/*
 *
 */
static DWORD
row2pcm(struct SoundBuffer *st, DWORD n)
{
  return n / st->block_align;
}

static DWORD
pcm2row(struct SoundBuffer *st, VALUE n)
{
  return n * st->block_align;
}

static VALUE
row2pcmnum(struct SoundBuffer *st, DWORD n)
{
  return UINT2NUM(row2pcm(st, n));
}

static DWORD
pcmnum2row(struct SoundBuffer *st, VALUE n)
{
  return pcm2row(st, NUM2UINT(n));
}

static DWORD
get_play_position(struct SoundBuffer *st)
{
  HRESULT   hr;
  DWORD     dwCurrentPlayCursor;
  DWORD     dwCurrentWriteCursor;

  hr = st->pDSBuffer8->lpVtbl->GetCurrentPosition(st->pDSBuffer8, &dwCurrentPlayCursor, &dwCurrentWriteCursor);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return dwCurrentPlayCursor;
}

static void
set_play_position(struct SoundBuffer *st, DWORD dwNewPosition)
{
  HRESULT   hr;

  hr = st->pDSBuffer8->lpVtbl->SetCurrentPosition(st->pDSBuffer8, dwNewPosition);
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
}

static VALUE
SoundBuffer_get_row_pos(VALUE self)
{
  return UINT2NUM(get_play_position(get_st(self)));
}

static VALUE
SoundBuffer_set_row_pos(VALUE self, VALUE voffset)
{
  struct SoundBuffer *st = get_st(self);

  set_play_position(st, NUM2UINT(voffset));
  return UINT2NUM(0);
}

static VALUE
SoundBuffer_get_pcm_pos(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return UINT2NUM(row2pcm(st, get_play_position(st)));
}

static VALUE
SoundBuffer_set_pcm_pos(VALUE self, VALUE voffset)
{
  struct SoundBuffer *st = get_st(self);

  set_play_position(st, pcm2row(st, NUM2UINT(voffset)));
  return UINT2NUM(0);
}

/*
 *
 */
static VALUE
SoundBuffer_size(VALUE self)
{
  return UINT2NUM(get_st(self)->buffer_bytes);
}

static VALUE
SoundBuffer_total(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return UINT2NUM(st->buffer_bytes / st->block_align);
}
/*
 *
 */
static VALUE
SoundBuffer_to_s(VALUE self)
{
  LPVOID   ptr1,  ptr2;
  DWORD    size1, size2;
  HRESULT  hr;
  VALUE    str;
  struct SoundBuffer *st = get_st(self);

// 再生中にto_sできるようにするか？
  if (get_playing(st)) rb_raise(eSoundBufferError, "now playing, plz stop");
  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, 0, 0, &ptr1, &size1, &ptr2, &size2, DSBLOCK_ENTIREBUFFER);
  if (FAILED(hr)) to_raise_an_exception(hr);
  if (size1 != st->buffer_bytes) {
    hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, 0, ptr2, 0);
    if (FAILED(hr)) to_raise_an_exception(hr);
    rb_raise(eSoundBufferError, "can not full size lock");
  }
  str = rb_str_new(ptr1, size1);
  if (rb_obj_tainted(self)) rb_obj_taint(str);
  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, 0, ptr2, 0);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return str;
}

/*
 *
 */
static VALUE
SoundBuffer_dispose(VALUE self)
{
  SoundBuffer_release(get_st(self));
  return self;
}

static VALUE
SoundBuffer_disposed(VALUE self)
{
  struct SoundBuffer *st = (struct SoundBuffer *)RTYPEDDATA_DATA(self);
  return st->pDSBuffer8 ? Qfalse : Qtrue;
}

/*
 *
 */
static VALUE
SoundBuffer_get_channels(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->channels));
}

static VALUE
SoundBuffer_get_samples_per_sec(VALUE self)
{
  return UINT2NUM(get_st(self)->samples_per_sec);
}

static VALUE
SoundBuffer_get_bits_per_sample(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->bits_per_sample));
}

static VALUE
SoundBuffer_get_block_align(VALUE self)
{
  return UINT2NUM((DWORD)(get_st(self)->block_align));
}

static VALUE
SoundBuffer_get_avg_bytes_per_sec(VALUE self)
{
  return UINT2NUM(get_st(self)->avg_bytes_per_sec);
}

static VALUE
SoundBuffer_get_effectable(VALUE self)
{
  return get_st(self)->effect_flag ? Qtrue : Qfalse;
}
/*
 * エフェクトパラメーター反映のためのメソッド
 * エフェクトのパラメーターは再生中に変更しても反映されない。
 * ここでは何もしないLock, Unlockを行うことで再生中にエフェクト・パラメーターの変更を強制的に反映させる。
 */
static VALUE
SoundBuffer_flash(VALUE self)
{
  HRESULT hr;
  LPVOID  ptr1,  ptr2;
  DWORD   size1, size2;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->Lock(st->pDSBuffer8, 0, 4, &ptr1, &size1, &ptr2, &size2, DSBLOCK_FROMWRITECURSOR);
  if (FAILED(hr)) to_raise_an_exception(hr);
  hr = st->pDSBuffer8->lpVtbl->Unlock(st->pDSBuffer8, ptr1, 0, ptr2, 0);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "unlock error");
  return self;
}

/*
 * nortify
 */
static HRESULT
notify_set_loop(struct SoundBuffer *st)
{
  HRESULT hr = 0;

  if (st->loop_flag) {
    if ( st->loop_count && st->loop_count > st->loop_counter) st->loop_counter += 1;
    if (!st->loop_count || st->loop_count > st->loop_counter) hr = st->pDSBuffer8->lpVtbl->SetCurrentPosition(st->pDSBuffer8, st->loop_start);
  }
  return hr;
}

static void*
notify_wait_blocking(void *data)
{
  struct NotifyData  *nd = data;
  struct SoundBuffer *st = nd->st;

  while (1) {
    nd->result = WaitForMultipleObjects(st->event_count, st->event_handles, FALSE, nd->timeout);
    /* DEBUG CODE
    if      (st->event_handles[nd->result - WAIT_OBJECT_0] == st->event_wait_break) printf("[WAIT_Break:%lu]", nd->result);
    else if (st->event_handles[nd->result - WAIT_OBJECT_0] == st->event_offsetstop) printf("[OFFSETSTOP:%lu]", nd->result);
    else if (st->event_handles[nd->result - WAIT_OBJECT_0] == st->event_loop_point) printf("[LOOP_Point:%lu]", nd->result);
    else printf("[USER_Event:%lu]", nd->result);
    */
    if (st->event_handles[nd->result - WAIT_OBJECT_0] == st->event_loop_point) notify_set_loop(st);
    else return NULL;
  }
}

static void
notify_wait_unblocking(void *data)
{
  struct NotifyData  *nd = data;
  struct SoundBuffer *st = nd->st;

  SetEvent(st->event_wait_break);
}

static VALUE
SoundBuffer_wait(int argc, VALUE *argv, VALUE self)
{
  struct NotifyData  data;
  struct SoundBuffer *st = get_st(self);

  if (argc > 1) rb_raise(rb_eArgError, "wrong number of arguments");

  data.result  = 0;
  data.st      = st;
  data.timeout = argc ? NUM2UINT(argv[0]) : INFINITE;
  while (1) {
    rb_thread_call_without_gvl(notify_wait_blocking, (void*)(&data), notify_wait_unblocking, (void*)(&data));
    if (st->event_handles[data.result - WAIT_OBJECT_0] != st->event_wait_break) break;
  }
  if (data.result == WAIT_FAILED)  rb_raise(eSoundBufferError, "[BUG]WaitForMultipleObjects error in notify_wait_blocking C function");
  if (data.result == WAIT_TIMEOUT) return Qnil;
  ResetEvent(st->event_handles[data.result - WAIT_OBJECT_0]);
  // OFFSETSTOP
  if (st->event_handles[data.result - WAIT_OBJECT_0] == st->event_offsetstop) {
    if (st->repeat_flag == 0 && get_play_position(st) == 0) SoundBuffer_stop(self);
    rb_raise(rb_eStopIteration, "OFFSETSTOP");
  }
  // User event
  return UINT2NUM(data.result - WAIT_OBJECT_0);
}

static LPHANDLE
notify_create_handles(struct SoundBuffer *st, DWORD count)
{
  LPHANDLE  handles;
  DWORD     i, j;

  handles = ALLOC_N(HANDLE, count);
  for (i = 0; i < count; i++) {
    if      (i == count - 1)  handles[i] = st->event_wait_break;
    else if (i == count - 2)  handles[i] = st->event_offsetstop;
    else if (i == count - 3)  handles[i] = st->event_loop_point;
    else                      handles[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (handles[i] == NULL) break;
  }
  if (i < count) {
    for (j = 0; j < i; j++) CloseHandle(handles[j]);
    xfree(handles);
    rb_raise(eSoundBufferError, "notify_create_handles error");
  }
  return handles;
}

static LPDWORD
notify_create_offsets(struct SoundBuffer *st, DWORD count, DWORD *dwoffsets)
{
  LPDWORD   offsets;
  DWORD     i;

  offsets = ALLOC_N(DWORD, count);
  for (i = 0; i < count; i++) {
    if      (i == count - 1)  offsets[i] = 0; // DUMMY
    else if (i == count - 2)  offsets[i] = DSBPN_OFFSETSTOP;
    else if (i == count - 3)  offsets[i] = st->loop_end;
    else                      offsets[i] = dwoffsets[i];
  }
  return offsets;
}

static HRESULT
notify_SetNotificationPositions(struct SoundBuffer *st, DWORD count, LPDWORD offsets, LPHANDLE handles)
{
  LPDIRECTSOUNDNOTIFY8  lpDsNotify;
  LPDSBPOSITIONNOTIFY   PositionNotify;
  DWORD                 i, notify_count;
  HRESULT               hr;

  hr = st->pDSBuffer8->lpVtbl->QueryInterface(st->pDSBuffer8, &IID_IDirectSoundNotify8, (LPVOID*)&lpDsNotify);
  if (SUCCEEDED(hr)) {
    // event_wait_breakはセットしない。よってcount - 1。また、サンプル位置０にnortifyをセットできる。
    notify_count = count - 1;
    PositionNotify = ALLOCA_N(DSBPOSITIONNOTIFY, notify_count);
    for (i = 0; i < notify_count; i++) {
      PositionNotify[i].dwOffset     = offsets[i];
      PositionNotify[i].hEventNotify = handles[i];
    }
    hr = lpDsNotify->lpVtbl->SetNotificationPositions(lpDsNotify, notify_count, PositionNotify);
  }
  lpDsNotify->lpVtbl->Release(lpDsNotify);
  return hr;
}

static void
create_st_event(struct SoundBuffer *st, DWORD argc, LPDWORD dwoffsets)
{
  LPHANDLE handles;
  LPDWORD  offsets;
  DWORD    i, count;
  HRESULT  hr;

  count   = argc + EVENT_PRESET;
  offsets = notify_create_offsets(st, count, dwoffsets);
  handles = notify_create_handles(st, count);
  hr = notify_SetNotificationPositions(st, count, offsets, handles);
  if (SUCCEEDED(hr)) {
    clear_st_event(st);
    st->event_count   = count;
    st->event_offsets = offsets;
    st->event_handles = handles;
  }
  else {
    xfree(offsets);
    for (i = 0; i < count - EVENT_PRESET; i++) CloseHandle(handles[i]);
    xfree(handles);
    to_raise_an_exception(hr);
  }
  PulseEvent(st->event_wait_break);
}

static VALUE
SoundBuffer_set_notify(int argc, VALUE *argv, VALUE self)
{
  int      i;
  LPDWORD  dwoffsets;
  struct SoundBuffer *st = get_st(self);

  dwoffsets = ALLOCA_N(DWORD, argc);
  for (i = 0; i < argc; i++) dwoffsets[i] = pcmnum2row(st, argv[i]);

  create_st_event(st, argc, dwoffsets);
  return self;
}

static VALUE
SoundBuffer_get_notify(VALUE self)
{
  DWORD i;
  VALUE ary;
  struct SoundBuffer *st = get_st(self);

  ary = rb_ary_new();
  if (st->event_count >= EVENT_PRESET) {
    for(i = 0; i < st->event_count - EVENT_PRESET; i++) rb_ary_store(ary, i, row2pcmnum(st, st->event_offsets[i]));
  }
  return ary;
}

/*
 *  sound play control
 */
static void
play_sound(struct SoundBuffer *st)
{
  HRESULT hr;

  hr = st->pDSBuffer8->lpVtbl->Play(st->pDSBuffer8, 0, 0, 0);
  if (FAILED(hr)) to_raise_an_exception(hr);
}

static void
repeat_sound(struct SoundBuffer *st)
{
  HRESULT hr;

  hr = st->pDSBuffer8->lpVtbl->Play(st->pDSBuffer8, 0, 0, DSBPLAY_LOOPING);
  if (FAILED(hr)) to_raise_an_exception(hr);
}

static void
stop_sound(struct SoundBuffer *st)
{
  HRESULT hr;

  hr = st->pDSBuffer8->lpVtbl->Stop(st->pDSBuffer8);
  if (FAILED(hr)) to_raise_an_exception(hr);
}

static VALUE
SoundBuffer_play(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  play_sound(st);
  st->play_flag    = 1;
  st->repeat_flag  = 0;
  return self;
}

static VALUE
SoundBuffer_repeat(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  repeat_sound(st);
  st->play_flag    = 1;
  st->repeat_flag  = 1;
  return self;
}

static VALUE
SoundBuffer_stop(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  stop_sound(st);
  set_play_position(st, 0);
  st->play_flag    = 0;
  st->repeat_flag  = 0;
  st->loop_counter = 0;
  return self;
}

static VALUE
SoundBuffer_pause(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  stop_sound(st);
  st->play_flag    = 0;
  return self;
}

/*
 *  sound play state request
 */
static DWORD
get_play_status(struct SoundBuffer *st)
{
  HRESULT hr;
  DWORD   dwStatus;

  hr = st->pDSBuffer8->lpVtbl->GetStatus(st->pDSBuffer8, &dwStatus);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return dwStatus;
}

/*
static void
reflect_play_status(struct SoundBuffer *st)
{
  DWORD   dwStatus;

  dwStatus = get_play_status(st);
  st->play_flag   = (dwStatus & DSBSTATUS_PLAYING) ? 1 : 0;
  st->repeat_flag = (dwStatus & DSBSTATUS_LOOPING) ? 1 : 0;
}
*/
static DWORD
get_playing(struct SoundBuffer *st)
{
  return (get_play_status(st) & DSBSTATUS_PLAYING) ? 1 : 0;
}

static VALUE
SoundBuffer_pausing(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return !st->play_flag && get_play_position(st) > 0 ? Qtrue : Qfalse;
}

static VALUE
SoundBuffer_playing(VALUE self)
{// 組み合わせ技でいく。
  struct SoundBuffer *st = get_st(self);

  return st->play_flag && get_playing(st) ? Qtrue : Qfalse;
}

static VALUE
SoundBuffer_repeating(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return st->repeat_flag ? Qtrue : Qfalse;
}

/*
 * Interval repeat
 */
static VALUE
SoundBuffer_get_loop(VALUE self)
{
  return get_st(self)->loop_flag ? Qtrue : Qfalse;
}

static VALUE
SoundBuffer_set_loop(VALUE self, VALUE v)
{
  get_st(self)->loop_flag = RTEST(v);
  return self;
}

static VALUE
SoundBuffer_set_loop_start(VALUE self, VALUE v)
{
  DWORD n;
  struct SoundBuffer *st = get_st(self);

  n = pcmnum2row(st, v);
  if (n > st->buffer_bytes) rb_raise(rb_eRangeError, "buffer_size");
  st->loop_start = n;
  return self;
}

static VALUE
SoundBuffer_set_loop_end(VALUE self, VALUE v)
{
  DWORD   n, argc;
  LPDWORD offsets;
  struct SoundBuffer *st = get_st(self);

  n = pcmnum2row(st, v);
  if (n > st->buffer_bytes) rb_raise(rb_eRangeError, "buffer_size");
  st->loop_end = n;

  argc = st->event_count - EVENT_PRESET;
  offsets = ALLOCA_N(DWORD, argc);
  MEMCPY(offsets, st->event_offsets, DWORD, argc);
  create_st_event(st, argc, offsets);

  return self;
}

static VALUE
SoundBuffer_set_loop_count(VALUE self, VALUE v)
{
  get_st(self)->loop_count = NUM2UINT(v);
  return self;
}

static VALUE
SoundBuffer_set_loop_counter(VALUE self, VALUE v)
{
  get_st(self)->loop_counter = NUM2UINT(v);
  return self;
}

static VALUE
SoundBuffer_get_loop_start(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return row2pcmnum(st, st->loop_start);
}

static VALUE
SoundBuffer_get_loop_end(VALUE self)
{
  struct SoundBuffer *st = get_st(self);

  return row2pcmnum(st, st->loop_end);
}

static VALUE
SoundBuffer_get_loop_count(VALUE self)
{
  return UINT2NUM(get_st(self)->loop_count);
}

static VALUE
SoundBuffer_get_loop_counter(VALUE self)
{
  return UINT2NUM(get_st(self)->loop_counter);
}
/*
 *
 */
static VALUE
SoundBuffer_stop_and_play(VALUE self)
{
  VALUE   result;
  DWORD   play_flag;
  struct SoundBuffer *st = get_st(self);

  play_flag = st->play_flag && get_playing(st);
  stop_sound(st);
  result = rb_block_given_p() ? rb_yield(self) : self;
  if (play_flag) st->repeat_flag ? repeat_sound(st) : play_sound(st);
  return result;
}

/*
 * SoundBuffer#get_volume
 */
static VALUE
SoundBuffer_get_volume(VALUE self)
{
  HRESULT hr;
  long    volume;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetVolume(st->pDSBuffer8, &volume);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return INT2NUM(volume);
}
/*
 * SoundBuffer#set_volume
 */
static VALUE
SoundBuffer_set_volume(VALUE self, VALUE vvolume)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetVolume(st->pDSBuffer8, NUM2INT(vvolume));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vvolume;
}
/*
 * SoundBuffer#get_pan
 */
static VALUE
SoundBuffer_get_pan(VALUE self)
{
  HRESULT hr;
  long    pan;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetPan(st->pDSBuffer8, &pan);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return INT2NUM(pan);
}
/*
 * SoundBuffer#set_pan
 */
static VALUE
SoundBuffer_set_pan(VALUE self, VALUE vpan)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetPan(st->pDSBuffer8, NUM2INT(vpan));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vpan;
}
/*
 * SoundBuffer#get_frequency
 */
static VALUE
SoundBuffer_get_frequency(VALUE self)
{
  HRESULT hr;
  unsigned long frequency;

  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetFrequency(st->pDSBuffer8, &frequency);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return UINT2NUM(frequency);
}
/*
 * SoundBuffer#set_frequency
 */
static VALUE
SoundBuffer_set_frequency(VALUE self, VALUE vfrequency)
{
  HRESULT hr;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->SetFrequency(st->pDSBuffer8, NUM2UINT(vfrequency));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vfrequency;
}
/*
 *
 */
static VALUE
SoundBuffer_get_effect(VALUE self)
{
  DWORD i;
  VALUE ary;
  struct SoundBuffer *st = get_st(self);

  if (!st->effect_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  ary = rb_ary_new();
  for (i = 0; i < st->effect_count; i++) rb_ary_store(ary, i, UINT2NUM(st->effect_nums[i]));
  return ary;
}

/*
 * set_effect
 * 再生中のエフェクトリスト変更はエラーになる
 */
static VALUE
SoundBuffer_set_effect(int argc, VALUE *argv, VALUE self)
{
  DWORD          i, count;
  GUID           guid;
  LPDSEFFECTDESC pDSFXDesc;
  HRESULT        hr;
  struct SoundBuffer *st = get_st(self);

  if (!st->effect_flag) rb_raise(rb_eNotImpError, "this object is not effect support");

  count = (DWORD)argc;
  if (count > 0) {
    pDSFXDesc = ALLOCA_N(DSEFFECTDESC, count);
    for (i = 0; i < count; i++) {
      switch (NUM2UINT(argv[i])) {
        case FX_GARGLE:
          guid = GUID_DSFX_STANDARD_GARGLE;
          break;
        case FX_CHORUS:
          guid = GUID_DSFX_STANDARD_CHORUS;
          break;
        case FX_FLANGER:
          guid = GUID_DSFX_STANDARD_FLANGER;
          break;
        case FX_ECHO:
          guid = GUID_DSFX_STANDARD_ECHO;
          break;
        case FX_DISTORTION:
          guid = GUID_DSFX_STANDARD_DISTORTION;
          break;
        case FX_COMPRESSOR:
          guid = GUID_DSFX_STANDARD_COMPRESSOR;
          break;
        case FX_PARAM_EQ:
          guid = GUID_DSFX_STANDARD_PARAMEQ;
          break;
        case FX_I3DL2_REVERB:
          guid = GUID_DSFX_STANDARD_I3DL2REVERB;
          break;
        case FX_WAVES_REVERB:
          guid = GUID_DSFX_WAVES_REVERB;
          break;
        default:
          rb_raise(rb_eTypeError, "not valid value");
          break;
      }
      pDSFXDesc[i].dwSize        = sizeof(DSEFFECTDESC);
      pDSFXDesc[i].dwFlags       = DSFX_LOCSOFTWARE;      // dwFlagは強制的にソフトウェアー配置
      pDSFXDesc[i].guidDSFXClass = guid;
      pDSFXDesc[i].dwReserved1   = 0;
      pDSFXDesc[i].dwReserved2   = 0;
    }
    hr = st->pDSBuffer8->lpVtbl->SetFX(st->pDSBuffer8, count, pDSFXDesc, NULL);
  }
  else hr = st->pDSBuffer8->lpVtbl->SetFX(st->pDSBuffer8,  0,      NULL, NULL);

  if (FAILED(hr)) to_raise_an_exception(hr);
  clear_st_effect(st);
  st->effect_nums  = count ? ALLOC_N(DWORD, count) : NULL;
  for (i = 0; i < count; i++) st->effect_nums[i] = NUM2UINT(argv[i]);
  st->effect_count = count;
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXGargle(VALUE self, VALUE nth)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXGargle  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_GARGLE,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXGargle8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXGargle8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXGargle8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(2,  UINT2NUM(dsfx.dwRateHz),
                                  UINT2NUM(dsfx.dwWaveShape));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXGargle(int argc, VALUE *argv, VALUE self)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXGargle  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 3) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.dwRateHz    = NUM2UINT(argv[1]);
  dsfx.dwWaveShape = NUM2UINT(argv[2]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_GARGLE,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXGargle8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXGargle8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXGargle8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXChorus(VALUE self, VALUE nth)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXChorus  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_CHORUS,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXChorus8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXChorus8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXChorus8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(7,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fDepth),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fFrequency),
                                  INT2NUM(dsfx.lWaveform),
                                  rb_float_new(dsfx.fDelay),
                                  INT2NUM(dsfx.lPhase));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXChorus(int argc, VALUE *argv, VALUE self)
{
  HRESULT     hr;
  LPVOID      pObject;
  DSFXChorus  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 8) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix = (float)NUM2DBL(argv[1]);
  dsfx.fDepth     = (float)NUM2DBL(argv[2]);
  dsfx.fFeedback  = (float)NUM2DBL(argv[3]);
  dsfx.fFrequency = (float)NUM2DBL(argv[4]);
  dsfx.lWaveform  = NUM2INT(argv[5]);
  dsfx.fDelay     = (float)NUM2DBL(argv[6]);
  dsfx.lPhase     = NUM2INT(argv[7]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_CHORUS,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXChorus8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXChorus8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXChorus8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXFlanger(VALUE self, VALUE nth)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXFlanger  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_FLANGER,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXFlanger8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXFlanger8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXFlanger8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(7,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fDepth),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fFrequency),
                                  INT2NUM(dsfx.lWaveform),
                                  rb_float_new(dsfx.fDelay),
                                  INT2NUM(dsfx.lPhase));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXFlanger(int argc, VALUE *argv, VALUE self)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXFlanger  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 8) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix = (float)NUM2DBL(argv[1]);
  dsfx.fDepth     = (float)NUM2DBL(argv[2]);
  dsfx.fFeedback  = (float)NUM2DBL(argv[3]);
  dsfx.fFrequency = (float)NUM2DBL(argv[4]);
  dsfx.lWaveform  = NUM2INT(argv[5]);
  dsfx.fDelay     = (float)NUM2DBL(argv[6]);
  dsfx.lPhase     = NUM2INT(argv[7]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_FLANGER,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXFlanger8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXFlanger8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXFlanger8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXEcho(VALUE self, VALUE nth)
{
  HRESULT   hr;
  LPVOID    pObject;
  DSFXEcho  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_ECHO,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXEcho8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXEcho8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXEcho8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(5,  rb_float_new(dsfx.fWetDryMix),
                                  rb_float_new(dsfx.fFeedback),
                                  rb_float_new(dsfx.fLeftDelay),
                                  rb_float_new(dsfx.fRightDelay),
                                  INT2NUM(dsfx.lPanDelay));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXEcho(int argc, VALUE *argv, VALUE self)
{
  HRESULT   hr;
  LPVOID    pObject;
  DSFXEcho  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 6) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fWetDryMix   = (float)NUM2DBL(argv[1]);
  dsfx.fFeedback    = (float)NUM2DBL(argv[2]);
  dsfx.fLeftDelay   = (float)NUM2DBL(argv[3]);
  dsfx.fRightDelay  = (float)NUM2DBL(argv[4]);
  dsfx.lPanDelay    = NUM2INT(argv[5]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_ECHO,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXEcho8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXEcho8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXEcho8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXDistortion(VALUE self, VALUE nth)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXDistortion  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_DISTORTION,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXDistortion8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXDistortion8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXDistortion8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(5,  rb_float_new(dsfx.fGain),
                                  rb_float_new(dsfx.fEdge),
                                  rb_float_new(dsfx.fPostEQCenterFrequency),
                                  rb_float_new(dsfx.fPostEQBandwidth),
                                  rb_float_new(dsfx.fPreLowpassCutoff));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXDistortion(int argc, VALUE *argv, VALUE self)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXDistortion  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 6) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fGain                  = (float)NUM2DBL(argv[1]);
  dsfx.fEdge                  = (float)NUM2DBL(argv[2]);
  dsfx.fPostEQCenterFrequency = (float)NUM2DBL(argv[3]);
  dsfx.fPostEQBandwidth       = (float)NUM2DBL(argv[4]);
  dsfx.fPreLowpassCutoff      = (float)NUM2DBL(argv[5]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_DISTORTION,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXDistortion8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXDistortion8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXDistortion8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXCompressor(VALUE self, VALUE nth)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXCompressor  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_COMPRESSOR,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXCompressor8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXCompressor8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXCompressor8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(6,  rb_float_new(dsfx.fGain),
                                  rb_float_new(dsfx.fAttack),
                                  rb_float_new(dsfx.fRelease),
                                  rb_float_new(dsfx.fThreshold),
                                  rb_float_new(dsfx.fRatio),
                                  rb_float_new(dsfx.fPredelay));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXCompressor(int argc, VALUE *argv, VALUE self)
{
  HRESULT         hr;
  LPVOID          pObject;
  DSFXCompressor  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 7) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fGain      = (float)NUM2DBL(argv[1]);
  dsfx.fAttack    = (float)NUM2DBL(argv[2]);
  dsfx.fRelease   = (float)NUM2DBL(argv[3]);
  dsfx.fThreshold = (float)NUM2DBL(argv[4]);
  dsfx.fRatio     = (float)NUM2DBL(argv[5]);
  dsfx.fPredelay  = (float)NUM2DBL(argv[6]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_COMPRESSOR,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXCompressor8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXCompressor8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXCompressor8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXParamEq(VALUE self, VALUE nth)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXParamEq  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_PARAMEQ,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXParamEq8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXParamEq8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXParamEq8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(3,  rb_float_new(dsfx.fCenter),
                                  rb_float_new(dsfx.fBandwidth),
                                  rb_float_new(dsfx.fGain));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXParamEq(int argc, VALUE *argv, VALUE self)
{
  HRESULT      hr;
  LPVOID       pObject;
  DSFXParamEq  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 4) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fCenter    = (float)NUM2DBL(argv[1]);
  dsfx.fBandwidth = (float)NUM2DBL(argv[2]);
  dsfx.fGain      = (float)NUM2DBL(argv[3]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_PARAMEQ,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXParamEq8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXParamEq8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXParamEq8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXI3DL2Reverb(VALUE self, VALUE nth)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXI3DL2Reverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_STANDARD_I3DL2REVERB,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXI3DL2Reverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXI3DL2Reverb8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXI3DL2Reverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(12, INT2NUM(dsfx.lRoom),
                                  INT2NUM(dsfx.lRoomHF),
                                  rb_float_new(dsfx.flRoomRolloffFactor),
                                  rb_float_new(dsfx.flDecayTime),
                                  rb_float_new(dsfx.flDecayHFRatio),
                                  INT2NUM(dsfx.lReflections),
                                  rb_float_new(dsfx.flReflectionsDelay),
                                  INT2NUM(dsfx.lReverb),
                                  rb_float_new(dsfx.flReverbDelay),
                                  rb_float_new(dsfx.flDiffusion),
                                  rb_float_new(dsfx.flDensity),
                                  rb_float_new(dsfx.flHFReference));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXI3DL2Reverb(int argc, VALUE *argv, VALUE self)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXI3DL2Reverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 13) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.lRoom               = NUM2INT(argv[1]);
  dsfx.lRoomHF             = NUM2INT(argv[2]);
  dsfx.flRoomRolloffFactor = (float)NUM2DBL(argv[3]);
  dsfx.flDecayTime         = (float)NUM2DBL(argv[4]);
  dsfx.flDecayHFRatio      = (float)NUM2DBL(argv[5]);
  dsfx.lReflections        = NUM2INT(argv[6]);
  dsfx.flReflectionsDelay  = (float)NUM2DBL(argv[7]);
  dsfx.lReverb             = NUM2INT(argv[8]);
  dsfx.flReverbDelay       = (float)NUM2DBL(argv[9]);
  dsfx.flDiffusion         = (float)NUM2DBL(argv[10]);
  dsfx.flDensity           = (float)NUM2DBL(argv[11]);
  dsfx.flHFReference       = (float)NUM2DBL(argv[12]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_STANDARD_I3DL2REVERB,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXI3DL2Reverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXI3DL2Reverb8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXI3DL2Reverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

static VALUE
SoundBuffer_GetAllParameters_DSFXWavesReverb(VALUE self, VALUE nth)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXWavesReverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8, &GUID_DSFX_WAVES_REVERB,
                                                NUM2UINT(nth),  &IID_IDirectSoundFXWavesReverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXWavesReverb8 *)pObject)->lpVtbl->GetAllParameters((struct IDirectSoundFXWavesReverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetAllParameters error");
  return rb_ary_new_from_args(4,  rb_float_new(dsfx.fInGain),
                                  rb_float_new(dsfx.fReverbMix),
                                  rb_float_new(dsfx.fReverbTime),
                                  rb_float_new(dsfx.fHighFreqRTRatio));
}

static VALUE
SoundBuffer_SetAllParameters_DSFXWavesReverb(int argc, VALUE *argv, VALUE self)
{
  HRESULT          hr;
  LPVOID           pObject;
  DSFXWavesReverb  dsfx;
  struct SoundBuffer *st = get_st(self);

  if (argc != 5) rb_raise(rb_eArgError, "Number of arguments does not match");
  dsfx.fInGain          = (float)NUM2DBL(argv[1]);
  dsfx.fReverbMix       = (float)NUM2DBL(argv[2]);
  dsfx.fReverbTime      = (float)NUM2DBL(argv[3]);
  dsfx.fHighFreqRTRatio = (float)NUM2DBL(argv[4]);
  hr = st->pDSBuffer8->lpVtbl->GetObjectInPath( st->pDSBuffer8,     &GUID_DSFX_WAVES_REVERB,
                                                NUM2UINT(argv[0]),  &IID_IDirectSoundFXWavesReverb8,
                                                &pObject);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "GetObjectInPath error");
  hr = ((struct IDirectSoundFXWavesReverb8 *)pObject)->lpVtbl->SetAllParameters((struct IDirectSoundFXWavesReverb8 *)pObject, &dsfx);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetAllParameters error");
  return self;
}

//
//
//
static VALUE
SoundBuffer_get_effect_param(VALUE self, VALUE nth)
{
  DWORD idx;
  struct SoundBuffer *st = get_st(self);

  if (!st->effect_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  idx = NUM2UINT(nth);
  if (idx > st->effect_count) rb_raise(rb_eRangeError, "effect index error");
  switch (st->effect_nums[idx]) {
    case FX_GARGLE:
      return SoundBuffer_GetAllParameters_DSFXGargle(self, nth);
      break;
    case FX_CHORUS:
      return SoundBuffer_GetAllParameters_DSFXChorus(self, nth);
      break;
    case FX_FLANGER:
      return SoundBuffer_GetAllParameters_DSFXFlanger(self, nth);
      break;
    case FX_ECHO:
      return SoundBuffer_GetAllParameters_DSFXEcho(self, nth);
      break;
    case FX_DISTORTION:
      return SoundBuffer_GetAllParameters_DSFXDistortion(self, nth);
      break;
    case FX_COMPRESSOR:
      return SoundBuffer_GetAllParameters_DSFXCompressor(self, nth);
      break;
    case FX_PARAM_EQ:
      return SoundBuffer_GetAllParameters_DSFXParamEq(self, nth);
      break;
    case FX_I3DL2_REVERB:
      return SoundBuffer_GetAllParameters_DSFXI3DL2Reverb(self, nth);
      break;
    case FX_WAVES_REVERB:
      return SoundBuffer_GetAllParameters_DSFXWavesReverb(self, nth);
      break;
    default:
      rb_raise(rb_eTypeError, "not valid value");
      break;
  }
}
//
//
//
static VALUE
SoundBuffer_set_effect_param(int argc, VALUE *argv, VALUE self)
{
  DWORD idx;
  struct SoundBuffer *st = get_st(self);

  if (!st->effect_flag) rb_raise(rb_eNotImpError, "this object is not effect support");
  idx = NUM2UINT(argv[0]);
  if (idx > st->effect_count) rb_raise(rb_eRangeError, "effect index error");
  switch (st->effect_nums[idx]) {
    case FX_GARGLE:
      return SoundBuffer_SetAllParameters_DSFXGargle(argc, argv, self);
      break;
    case FX_CHORUS:
      return SoundBuffer_SetAllParameters_DSFXChorus(argc, argv, self);
      break;
    case FX_FLANGER:
      return SoundBuffer_SetAllParameters_DSFXFlanger(argc, argv, self);
      break;
    case FX_ECHO:
      return SoundBuffer_SetAllParameters_DSFXEcho(argc, argv, self);
      break;
    case FX_DISTORTION:
      return SoundBuffer_SetAllParameters_DSFXDistortion(argc, argv, self);
      break;
    case FX_COMPRESSOR:
      return SoundBuffer_SetAllParameters_DSFXCompressor(argc, argv, self);
      break;
    case FX_PARAM_EQ:
      return SoundBuffer_SetAllParameters_DSFXParamEq(argc, argv, self);
      break;
    case FX_I3DL2_REVERB:
      return SoundBuffer_SetAllParameters_DSFXI3DL2Reverb(argc, argv, self);
      break;
    case FX_WAVES_REVERB:
      return SoundBuffer_SetAllParameters_DSFXWavesReverb(argc, argv, self);
      break;
    default:
      rb_raise(rb_eTypeError, "not valid value");
      break;
  }
}

/*
 * class singleton methods
 */
static VALUE
SoundBuffer_c_get_format(VALUE self)
{
  WAVEFORMATEX  pcmwf;
  DWORD         wSizeWritten;
  HRESULT       hr;

  hr = g_pDSBuffer->lpVtbl->GetFormat(g_pDSBuffer, NULL, 0, &wSizeWritten);
  if (FAILED(hr)) to_raise_an_exception(hr);
  if (wSizeWritten != sizeof(WAVEFORMATEX)) rb_raise(eSoundBufferError, "not support foramt");
  hr = g_pDSBuffer->lpVtbl->GetFormat(g_pDSBuffer, &pcmwf, sizeof(WAVEFORMATEX), NULL);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return rb_ary_new_from_args(3,  UINT2NUM((DWORD)pcmwf.nChannels),
                                  UINT2NUM(       pcmwf.nSamplesPerSec),
                                  UINT2NUM((DWORD)pcmwf.wBitsPerSample));
}

static VALUE
SoundBuffer_c_set_format(VALUE self, VALUE vchannels, VALUE vsamples_per_sec, VALUE vbits_per_sample)
{
  WAVEFORMATEX  pcmwf;
  HRESULT       hr;
  WORD          channels, bits_per_sample;
  DWORD         samples_per_sec;

  channels        = (WORD)NUM2UINT(vchannels);
  if (channels != 1 && channels != 2) rb_raise(rb_eRangeError, "channels arguments 1 and 2 only possible");
  samples_per_sec =       NUM2UINT(vsamples_per_sec);
  if (samples_per_sec < DSBFREQUENCY_MIN || DSBFREQUENCY_MAX < samples_per_sec) rb_raise(rb_eRangeError, "samples_per_sec argument can be only DSBFREQUENCY_MIN-DSBFREQUENCY_MAX");
  bits_per_sample = (WORD)NUM2UINT(vbits_per_sample);
  if (bits_per_sample != 8 && bits_per_sample != 16) rb_raise(rb_eRangeError, "bits_per_sample arguments 8 and 16 only possible");

  pcmwf.wFormatTag      = WAVE_FORMAT_PCM;
  pcmwf.nChannels       = channels;
  pcmwf.nSamplesPerSec  = samples_per_sec;
  pcmwf.wBitsPerSample  = bits_per_sample;
  pcmwf.nBlockAlign     = bits_per_sample / 8 * channels;
  pcmwf.nAvgBytesPerSec = samples_per_sec * pcmwf.nBlockAlign;
  pcmwf.cbSize          = 0;
  hr = g_pDSBuffer->lpVtbl->SetFormat(g_pDSBuffer, &pcmwf);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return self;
}

static VALUE
SoundBuffer_c_get_volume(VALUE self)
{
  HRESULT hr;
  long    volume;

  hr = g_pDSBuffer->lpVtbl->GetVolume(g_pDSBuffer, &volume);
  if (FAILED(hr)) to_raise_an_exception(hr);
  return INT2NUM(volume);
}

static VALUE
SoundBuffer_c_set_volume(VALUE self, VALUE vvolume)
{
  HRESULT hr;

  hr = g_pDSBuffer->lpVtbl->SetVolume(g_pDSBuffer, NUM2INT(vvolume));
  if (hr == DSERR_INVALIDPARAM) rb_raise(rb_eRangeError, "DSERR_INVALIDPARAM error");
  if (FAILED(hr)) to_raise_an_exception(hr);
  return vvolume;
}

// Rubyのクラス定義
void
Init_SoundBuffer(void)
{
  eSoundBufferError = rb_define_class("SoundBufferError", rb_eRuntimeError);

  cSoundBuffer = rb_define_class("SoundBuffer", rb_cObject);

  rb_define_alloc_func(cSoundBuffer, SoundBuffer_allocate);
  rb_define_singleton_method(cSoundBuffer, "get_format", SoundBuffer_c_get_format,   0);
  rb_define_singleton_method(cSoundBuffer, "set_format", SoundBuffer_c_set_format,   3);
  rb_define_singleton_method(cSoundBuffer, "get_volume", SoundBuffer_c_get_volume,   0);
  rb_define_singleton_method(cSoundBuffer, "set_volume", SoundBuffer_c_set_volume,   1);

  rb_define_method(cSoundBuffer, "initialize",        SoundBuffer_initialize,       -1);
  rb_define_method(cSoundBuffer, "initialize_copy",   SoundBuffer_initialize_copy,   1);

  rb_define_method(cSoundBuffer, "wait",              SoundBuffer_wait,             -1);
  rb_define_method(cSoundBuffer, "get_notify",        SoundBuffer_get_notify,        0);
  rb_define_method(cSoundBuffer, "set_notify",        SoundBuffer_set_notify,       -1);

  rb_define_method(cSoundBuffer, "dispose",           SoundBuffer_dispose,           0);
  rb_define_method(cSoundBuffer, "disposed?",         SoundBuffer_disposed,          0);
  rb_define_method(cSoundBuffer, "flash",             SoundBuffer_flash,             0);
  rb_define_method(cSoundBuffer, "origin?",           SoundBuffer_get_origin,        0);
  rb_define_method(cSoundBuffer, "pause",             SoundBuffer_pause,             0);
  rb_define_method(cSoundBuffer, "pausing?",          SoundBuffer_pausing,           0);
  rb_define_method(cSoundBuffer, "play",              SoundBuffer_play,              0);
  rb_define_method(cSoundBuffer, "playing?",          SoundBuffer_playing,           0);
  rb_define_method(cSoundBuffer, "repeat",            SoundBuffer_repeat,            0);
  rb_define_method(cSoundBuffer, "repeating?",        SoundBuffer_repeating,         0);
  rb_define_method(cSoundBuffer, "size",              SoundBuffer_size,              0);
  rb_define_method(cSoundBuffer, "stop",              SoundBuffer_stop,              0);
  rb_define_method(cSoundBuffer, "stop_and_play",     SoundBuffer_stop_and_play,     0);
  rb_define_method(cSoundBuffer, "to_s",              SoundBuffer_to_s,              0);
  rb_define_method(cSoundBuffer, "total",             SoundBuffer_total,             0);
  rb_define_method(cSoundBuffer, "write",             SoundBuffer_write,            -1);
  rb_define_method(cSoundBuffer, "effectable?",       SoundBuffer_get_effectable,    0);

  rb_define_method(cSoundBuffer, "channels",          SoundBuffer_get_channels,          0);
  rb_define_method(cSoundBuffer, "samples_per_sec",   SoundBuffer_get_samples_per_sec,   0);
  rb_define_method(cSoundBuffer, "bits_per_sample",   SoundBuffer_get_bits_per_sample,   0);
  rb_define_method(cSoundBuffer, "block_align",       SoundBuffer_get_block_align,       0);
  rb_define_method(cSoundBuffer, "avg_bytes_per_sec", SoundBuffer_get_avg_bytes_per_sec, 0);

  rb_define_method(cSoundBuffer, "loop?",             SoundBuffer_get_loop,          0);
  rb_define_method(cSoundBuffer, "set_loop",          SoundBuffer_set_loop,          1);
  rb_define_method(cSoundBuffer, "loop_count",        SoundBuffer_get_loop_count,    0);
  rb_define_method(cSoundBuffer, "loop_count=",       SoundBuffer_set_loop_count,    1);
  rb_define_method(cSoundBuffer, "loop_counter",      SoundBuffer_get_loop_counter,  0);
  rb_define_method(cSoundBuffer, "loop_counter=",     SoundBuffer_set_loop_counter,  1);
  rb_define_method(cSoundBuffer, "loop_start",        SoundBuffer_get_loop_start,    0);
  rb_define_method(cSoundBuffer, "loop_start=",       SoundBuffer_set_loop_start,    1);
  rb_define_method(cSoundBuffer, "loop_end",          SoundBuffer_get_loop_end,      0);
  rb_define_method(cSoundBuffer, "loop_end=",         SoundBuffer_set_loop_end,      1);

  rb_define_method(cSoundBuffer, "pcm_pos",           SoundBuffer_get_pcm_pos,       0);
  rb_define_method(cSoundBuffer, "pcm_pos=",          SoundBuffer_set_pcm_pos,       1);
  rb_define_method(cSoundBuffer, "row_pos",           SoundBuffer_get_row_pos,       0);
  rb_define_method(cSoundBuffer, "row_pos=",          SoundBuffer_set_row_pos,       1);
  rb_define_method(cSoundBuffer, "get_volume",        SoundBuffer_get_volume,        0);
  rb_define_method(cSoundBuffer, "set_volume",        SoundBuffer_set_volume,        1);
  rb_define_method(cSoundBuffer, "get_pan",           SoundBuffer_get_pan,           0);
  rb_define_method(cSoundBuffer, "set_pan",           SoundBuffer_set_pan,           1);
  rb_define_method(cSoundBuffer, "get_frequency",     SoundBuffer_get_frequency,     0);
  rb_define_method(cSoundBuffer, "set_frequency",     SoundBuffer_set_frequency,     1);
  rb_define_method(cSoundBuffer, "get_effect",        SoundBuffer_get_effect,        0);
  rb_define_method(cSoundBuffer, "set_effect",        SoundBuffer_set_effect,       -1);
  rb_define_method(cSoundBuffer, "get_effect_param",  SoundBuffer_get_effect_param,  1);
  rb_define_method(cSoundBuffer, "set_effect_param",  SoundBuffer_set_effect_param, -1);

  rb_define_alias(cSoundBuffer, "volume",     "get_volume");
  rb_define_alias(cSoundBuffer, "volume=",    "set_volume");
  rb_define_alias(cSoundBuffer, "pan",        "get_pan");
  rb_define_alias(cSoundBuffer, "pan=",       "set_pan");
  rb_define_alias(cSoundBuffer, "frequency",  "get_frequency");
  rb_define_alias(cSoundBuffer, "frequency=", "set_frequency");
  rb_define_alias(cSoundBuffer, "pos",        "row_pos");
  rb_define_alias(cSoundBuffer, "pos=",       "row_pos=");
  rb_define_alias(cSoundBuffer, "play_pos",   "pcm_pos");
  rb_define_alias(cSoundBuffer, "play_pos=",  "pcm_pos=");

  /*
   * Consts
   */
  rb_define_const(cSoundBuffer, "FX_GARGLE",       INT2NUM(FX_GARGLE));
  rb_define_const(cSoundBuffer, "FX_CHORUS",       INT2NUM(FX_CHORUS));
  rb_define_const(cSoundBuffer, "FX_FLANGER",      INT2NUM(FX_FLANGER));
  rb_define_const(cSoundBuffer, "FX_ECHO",         INT2NUM(FX_ECHO));
  rb_define_const(cSoundBuffer, "FX_DISTORTION",   INT2NUM(FX_DISTORTION));
  rb_define_const(cSoundBuffer, "FX_COMPRESSOR",   INT2NUM(FX_COMPRESSOR));
  rb_define_const(cSoundBuffer, "FX_PARAM_EQ",     INT2NUM(FX_PARAM_EQ));
  rb_define_const(cSoundBuffer, "FX_I3DL2_REVERB", INT2NUM(FX_I3DL2_REVERB));
  rb_define_const(cSoundBuffer, "FX_WAVES_REVERB", INT2NUM(FX_WAVES_REVERB));
  /*
   * IDirectSoundFXGargle
   */
  rb_define_const(cSoundBuffer, "DSFXGARGLE_WAVE_TRIANGLE",                   INT2NUM(DSFXGARGLE_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_WAVE_SQUARE",                     INT2NUM(DSFXGARGLE_WAVE_SQUARE));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_RATEHZ_MIN",                      INT2NUM(DSFXGARGLE_RATEHZ_MIN));
  rb_define_const(cSoundBuffer, "DSFXGARGLE_RATEHZ_MAX",                      INT2NUM(DSFXGARGLE_RATEHZ_MAX));
  /*
   * IDirectSoundFXChorus
   */
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WAVE_TRIANGLE",                   INT2NUM(DSFXCHORUS_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WAVE_SIN",                        INT2NUM(DSFXCHORUS_WAVE_SIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WETDRYMIX_MIN",                   rb_float_new(DSFXCHORUS_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_WETDRYMIX_MAX",                   rb_float_new(DSFXCHORUS_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DEPTH_MIN",                       rb_float_new(DSFXCHORUS_DEPTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DEPTH_MAX",                       rb_float_new(DSFXCHORUS_DEPTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FEEDBACK_MIN",                    rb_float_new(DSFXCHORUS_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FEEDBACK_MAX",                    rb_float_new(DSFXCHORUS_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FREQUENCY_MIN",                   rb_float_new(DSFXCHORUS_FREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_FREQUENCY_MAX",                   rb_float_new(DSFXCHORUS_FREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DELAY_MIN",                       rb_float_new(DSFXCHORUS_DELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_DELAY_MAX",                       rb_float_new(DSFXCHORUS_DELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_MIN",                       INT2NUM(DSFXCHORUS_PHASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_MAX",                       INT2NUM(DSFXCHORUS_PHASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_NEG_180",                   INT2NUM(DSFXCHORUS_PHASE_NEG_180));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_NEG_90",                    INT2NUM(DSFXCHORUS_PHASE_NEG_90));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_ZERO",                      INT2NUM(DSFXCHORUS_PHASE_ZERO));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_90",                        INT2NUM(DSFXCHORUS_PHASE_90));
  rb_define_const(cSoundBuffer, "DSFXCHORUS_PHASE_180",                       INT2NUM(DSFXCHORUS_PHASE_180));
  /*
   * IDirectSoundFXFlanger
   */
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WAVE_TRIANGLE",                  INT2NUM(DSFXFLANGER_WAVE_TRIANGLE));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WAVE_SIN",                       INT2NUM(DSFXFLANGER_WAVE_SIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WETDRYMIX_MIN",                  rb_float_new(DSFXFLANGER_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_WETDRYMIX_MAX",                  rb_float_new(DSFXFLANGER_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FREQUENCY_MIN",                  rb_float_new(DSFXFLANGER_FREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FREQUENCY_MAX",                  rb_float_new(DSFXFLANGER_FREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DEPTH_MIN",                      rb_float_new(DSFXFLANGER_DEPTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DEPTH_MAX",                      rb_float_new(DSFXFLANGER_DEPTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_MIN",                      INT2NUM(DSFXFLANGER_PHASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_MAX",                      INT2NUM(DSFXFLANGER_PHASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FEEDBACK_MIN",                   rb_float_new(DSFXFLANGER_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_FEEDBACK_MAX",                   rb_float_new(DSFXFLANGER_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DELAY_MIN",                      rb_float_new(DSFXFLANGER_DELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_DELAY_MAX",                      rb_float_new(DSFXFLANGER_DELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_NEG_180",                  INT2NUM(DSFXFLANGER_PHASE_NEG_180));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_NEG_90",                   INT2NUM(DSFXFLANGER_PHASE_NEG_90));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_ZERO",                     INT2NUM(DSFXFLANGER_PHASE_ZERO));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_90",                       INT2NUM(DSFXFLANGER_PHASE_90));
  rb_define_const(cSoundBuffer, "DSFXFLANGER_PHASE_180",                      INT2NUM(DSFXFLANGER_PHASE_180));
  /*
   * IDirectSoundFXEcho
   */
  rb_define_const(cSoundBuffer, "DSFXECHO_WETDRYMIX_MIN",                     rb_float_new(DSFXECHO_WETDRYMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_WETDRYMIX_MAX",                     rb_float_new(DSFXECHO_WETDRYMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_FEEDBACK_MIN",                      rb_float_new(DSFXECHO_FEEDBACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_FEEDBACK_MAX",                      rb_float_new(DSFXECHO_FEEDBACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_LEFTDELAY_MIN",                     rb_float_new(DSFXECHO_LEFTDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_LEFTDELAY_MAX",                     rb_float_new(DSFXECHO_LEFTDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_RIGHTDELAY_MIN",                    rb_float_new(DSFXECHO_RIGHTDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_RIGHTDELAY_MAX",                    rb_float_new(DSFXECHO_RIGHTDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFXECHO_PANDELAY_MIN",                      INT2NUM(DSFXECHO_PANDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXECHO_PANDELAY_MAX",                      INT2NUM(DSFXECHO_PANDELAY_MAX));
  /*
   * IDirectSoundFXDistortion
   */
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_GAIN_MIN",                    rb_float_new(DSFXDISTORTION_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_GAIN_MAX",                    rb_float_new(DSFXDISTORTION_GAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_EDGE_MIN",                    rb_float_new(DSFXDISTORTION_EDGE_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_EDGE_MAX",                    rb_float_new(DSFXDISTORTION_EDGE_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQCENTERFREQUENCY_MIN",   rb_float_new(DSFXDISTORTION_POSTEQCENTERFREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQCENTERFREQUENCY_MAX",   rb_float_new(DSFXDISTORTION_POSTEQCENTERFREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQBANDWIDTH_MIN",         rb_float_new(DSFXDISTORTION_POSTEQBANDWIDTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_POSTEQBANDWIDTH_MAX",         rb_float_new(DSFXDISTORTION_POSTEQBANDWIDTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_PRELOWPASSCUTOFF_MIN",        rb_float_new(DSFXDISTORTION_PRELOWPASSCUTOFF_MIN));
  rb_define_const(cSoundBuffer, "DSFXDISTORTION_PRELOWPASSCUTOFF_MAX",        rb_float_new(DSFXDISTORTION_PRELOWPASSCUTOFF_MAX));
  /*
   * IDirectSoundFXCompressor
   */
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_GAIN_MIN",                    rb_float_new(DSFXCOMPRESSOR_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_GAIN_MAX",                    rb_float_new(DSFXCOMPRESSOR_GAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_ATTACK_MIN",                  rb_float_new(DSFXCOMPRESSOR_ATTACK_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_ATTACK_MAX",                  rb_float_new(DSFXCOMPRESSOR_ATTACK_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RELEASE_MIN",                 rb_float_new(DSFXCOMPRESSOR_RELEASE_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RELEASE_MAX",                 rb_float_new(DSFXCOMPRESSOR_RELEASE_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_THRESHOLD_MIN",               rb_float_new(DSFXCOMPRESSOR_THRESHOLD_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_THRESHOLD_MAX",               rb_float_new(DSFXCOMPRESSOR_THRESHOLD_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RATIO_MIN",                   rb_float_new(DSFXCOMPRESSOR_RATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_RATIO_MAX",                   rb_float_new(DSFXCOMPRESSOR_RATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_PREDELAY_MIN",                rb_float_new(DSFXCOMPRESSOR_PREDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFXCOMPRESSOR_PREDELAY_MAX",                rb_float_new(DSFXCOMPRESSOR_PREDELAY_MAX));
  /*
   * IDirectSoundFXParamEq
   */
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_CENTER_MIN",                     rb_float_new(DSFXPARAMEQ_CENTER_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_CENTER_MAX",                     rb_float_new(DSFXPARAMEQ_CENTER_MAX));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_BANDWIDTH_MIN",                  rb_float_new(DSFXPARAMEQ_BANDWIDTH_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_BANDWIDTH_MAX",                  rb_float_new(DSFXPARAMEQ_BANDWIDTH_MAX));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_GAIN_MIN",                       rb_float_new(DSFXPARAMEQ_GAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFXPARAMEQ_GAIN_MAX",                       rb_float_new(DSFXPARAMEQ_GAIN_MAX));
  /*
   * IDirectSoundFXI3DL2Reverb
   */
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_MIN",                  INT2NUM(DSFX_I3DL2REVERB_ROOM_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_MAX",                  INT2NUM(DSFX_I3DL2REVERB_ROOM_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOM_DEFAULT",              INT2NUM(DSFX_I3DL2REVERB_ROOM_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_MIN",                INT2NUM(DSFX_I3DL2REVERB_ROOMHF_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_MAX",                INT2NUM(DSFX_I3DL2REVERB_ROOMHF_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMHF_DEFAULT",            INT2NUM(DSFX_I3DL2REVERB_ROOMHF_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN",     rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX",     rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_DEFAULT", rb_float_new(DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_MIN",             rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_MAX",             rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYTIME_DEFAULT",         rb_float_new(DSFX_I3DL2REVERB_DECAYTIME_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_MIN",          rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_MAX",          rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DECAYHFRATIO_DEFAULT",      rb_float_new(DSFX_I3DL2REVERB_DECAYHFRATIO_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_MIN",           INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_MAX",           INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONS_DEFAULT",       INT2NUM(DSFX_I3DL2REVERB_REFLECTIONS_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN",      rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_MAX",      rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REFLECTIONSDELAY_DEFAULT",  rb_float_new(DSFX_I3DL2REVERB_REFLECTIONSDELAY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_MIN",                INT2NUM(DSFX_I3DL2REVERB_REVERB_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_MAX",                INT2NUM(DSFX_I3DL2REVERB_REVERB_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERB_DEFAULT",            INT2NUM(DSFX_I3DL2REVERB_REVERB_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_MIN",           rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_MAX",           rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_REVERBDELAY_DEFAULT",       rb_float_new(DSFX_I3DL2REVERB_REVERBDELAY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_MIN",             rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_MAX",             rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DIFFUSION_DEFAULT",         rb_float_new(DSFX_I3DL2REVERB_DIFFUSION_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_MIN",               rb_float_new(DSFX_I3DL2REVERB_DENSITY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_MAX",               rb_float_new(DSFX_I3DL2REVERB_DENSITY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_DENSITY_DEFAULT",           rb_float_new(DSFX_I3DL2REVERB_DENSITY_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_MIN",           rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_MAX",           rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_HFREFERENCE_DEFAULT",       rb_float_new(DSFX_I3DL2REVERB_HFREFERENCE_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_MIN",               INT2NUM(DSFX_I3DL2REVERB_QUALITY_MIN));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_MAX",               INT2NUM(DSFX_I3DL2REVERB_QUALITY_MAX));
  rb_define_const(cSoundBuffer, "DSFX_I3DL2REVERB_QUALITY_DEFAULT",           INT2NUM(DSFX_I3DL2REVERB_QUALITY_DEFAULT));
  /*
   * IDirectSoundFXWavesReverb
   */
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_MIN",                rb_float_new(DSFX_WAVESREVERB_INGAIN_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_MAX",                rb_float_new(DSFX_WAVESREVERB_INGAIN_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_INGAIN_DEFAULT",            rb_float_new(DSFX_WAVESREVERB_INGAIN_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_MIN",             rb_float_new(DSFX_WAVESREVERB_REVERBMIX_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_MAX",             rb_float_new(DSFX_WAVESREVERB_REVERBMIX_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBMIX_DEFAULT",         rb_float_new(DSFX_WAVESREVERB_REVERBMIX_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_MIN",            rb_float_new(DSFX_WAVESREVERB_REVERBTIME_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_MAX",            rb_float_new(DSFX_WAVESREVERB_REVERBTIME_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_REVERBTIME_DEFAULT",        rb_float_new(DSFX_WAVESREVERB_REVERBTIME_DEFAULT));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_MIN",       rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_MIN));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_MAX",       rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_MAX));
  rb_define_const(cSoundBuffer, "DSFX_WAVESREVERB_HIGHFREQRTRATIO_DEFAULT",   rb_float_new(DSFX_WAVESREVERB_HIGHFREQRTRATIO_DEFAULT));
  /*
   * Flags
   */
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_ORIGINAL",                      INT2NUM(DSBFREQUENCY_ORIGINAL));
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_MIN",                           INT2NUM(DSBFREQUENCY_MIN));
  rb_define_const(cSoundBuffer, "DSBFREQUENCY_MAX",                           INT2NUM(DSBFREQUENCY_MAX));
  rb_define_const(cSoundBuffer, "DSBPAN_LEFT",                                INT2NUM(DSBPAN_LEFT));
  rb_define_const(cSoundBuffer, "DSBPAN_CENTER",                              INT2NUM(DSBPAN_CENTER));
  rb_define_const(cSoundBuffer, "DSBPAN_RIGHT",                               INT2NUM(DSBPAN_RIGHT));
  rb_define_const(cSoundBuffer, "DSBVOLUME_MIN",                              INT2NUM(DSBVOLUME_MIN));
  rb_define_const(cSoundBuffer, "DSBVOLUME_MAX",                              INT2NUM(DSBVOLUME_MAX));
  rb_define_const(cSoundBuffer, "DSBSIZE_MIN",                                INT2NUM(DSBSIZE_MIN));
  rb_define_const(cSoundBuffer, "DSBSIZE_MAX",                                INT2NUM(DSBSIZE_MAX));
  rb_define_const(cSoundBuffer, "DSBSIZE_FX_MIN",                             INT2NUM(DSBSIZE_FX_MIN));
  rb_define_const(cSoundBuffer, "DSBNOTIFICATIONS_MAX",                       INT2NUM(DSBNOTIFICATIONS_MAX));
}

// 終了時に実行されるENDブロックに登録する関数
static void SoundBuffer_shutdown(VALUE obj)
{
  g_pDSBuffer->lpVtbl->Release(g_pDSBuffer);
  g_refcount--;

  // shutduwn+すべてのSoundTestが解放されたらDirectSound解放
  g_refcount--;
  if (g_refcount == 0) {
    g_pDSound->lpVtbl->Release(g_pDSound);
    CoUninitialize();
  }
}

void Init_soundbuffer(void)
{
  HWND          hWnd;
  HINSTANCE     hInstance;
  WNDCLASSEX    wcex;
  DSBUFFERDESC  desc;
  HRESULT       hr;

  // COM初期化
  CoInitialize(NULL);

  // ウィンドウクラス設定
  hInstance = (HINSTANCE)GetModuleHandle(NULL);
  wcex.cbSize        = sizeof(WNDCLASSEX);
  wcex.style         = 0;
  wcex.lpfnWndProc   = DefWindowProc;
  wcex.cbClsExtra    = 0;
  wcex.cbWndExtra    = 0;
  wcex.hInstance     = hInstance;
  wcex.hIcon         = 0;
  wcex.hIconSm       = 0;
  wcex.hCursor       = 0;
  wcex.hbrBackground = 0;
  wcex.lpszMenuName  = NULL;
  wcex.lpszClassName = "SoundBuffer";

  // ウィンドウ生成
  RegisterClassEx(&wcex);
  hWnd = CreateWindow("SoundBuffer", "", 0, 0, 0, 0, 0, 0, NULL, hInstance, NULL);

  // DirectSoundオブジェクト生成
  hr = DirectSoundCreate8(&DSDEVID_DefaultPlayback, &g_pDSound, NULL);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "DirectSoundCreate8 error");

  // 協調レベル設定
  hr = g_pDSound->lpVtbl->SetCooperativeLevel(g_pDSound, hWnd, DSSCL_PRIORITY);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "SetCooperativeLevel error");

  g_refcount++;

  /*
   *  get primarybuffer
   */
  desc.dwSize           = sizeof(DSBUFFERDESC);
  desc.dwFlags          = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME;
  desc.dwBufferBytes    = 0;
  desc.dwReserved       = 0;
  desc.lpwfxFormat      = NULL;
  desc.guid3DAlgorithm  = DS3DALG_DEFAULT;

  hr = g_pDSound->lpVtbl->CreateSoundBuffer(g_pDSound, &desc, &g_pDSBuffer, NULL);
  if (FAILED(hr)) rb_raise(eSoundBufferError, "get PrimaryBuffer error");

  g_refcount++;

  // 終了時に実行する関数
  rb_set_end_proc(SoundBuffer_shutdown, Qnil);

  // SoundTestクラス生成
  Init_SoundBuffer();
}

static void
to_raise_an_exception(HRESULT hr)
{ //http://clalance.blogspot.jp/2011/01/writing-ruby-extensions-in-c-part-5.html
  switch (hr) {
    //case DS_OK:
    //  rb_raise(eSoundBufferError, "DS_OK");
    //  break;
    case DS_NO_VIRTUALIZATION:
      rb_raise(eSoundBufferError, "DS_NO_VIRTUALIZATION");
      break;
    //case DS_INCOMPLETE:
    //  rb_raise(eSoundBufferError, "DS_INCOMPLETE");
    //  break;
    case DSERR_ACCESSDENIED:
      rb_raise(eSoundBufferError, "DSERR_ACCESSDENIED");
      break;
    case DSERR_ALLOCATED:
      rb_raise(eSoundBufferError, "DSERR_ALLOCATED");
      break;
    case DSERR_ALREADYINITIALIZED:
      rb_raise(eSoundBufferError, "DSERR_ALREADYINITIALIZED");
      break;
    case DSERR_BADFORMAT:
      rb_raise(eSoundBufferError, "DSERR_BADFORMAT");
      break;
    case DSERR_BADSENDBUFFERGUID:
      rb_raise(eSoundBufferError, "DSERR_BADSENDBUFFERGUID");
      break;
    case DSERR_BUFFERLOST:
      /*
       * LOCALSOFTWAREに置かれたバッファーがロストするか不明
       * このエラーを報告するようにしてある。これが発生したら報告してほしい。
       */
      rb_raise(eSoundBufferError, "DSERR_BUFFERLOST");
      break;
    case DSERR_BUFFERTOOSMALL:
      rb_raise(eSoundBufferError, "DSERR_BUFFERTOOSMALL");
      break;
    case DSERR_CONTROLUNAVAIL:
      rb_raise(eSoundBufferError, "DSERR_CONTROLUNAVAIL");
      break;
    case DSERR_DS8_REQUIRED:
      rb_raise(eSoundBufferError, "DSERR_DS8_REQUIRED");
      break;
    case DSERR_FXUNAVAILABLE:
      rb_raise(eSoundBufferError, "DSERR_FXUNAVAILABLE");
      break;
    case DSERR_GENERIC:
      rb_raise(eSoundBufferError, "DSERR_GENERIC");
      break;
    case DSERR_INVALIDCALL:
      rb_raise(eSoundBufferError, "DSERR_INVALIDCALL");
      break;
    case DSERR_INVALIDPARAM:
      rb_raise(eSoundBufferError, "DSERR_INVALIDPARAM");
      break;
    case DSERR_NOAGGREGATION:
      rb_raise(eSoundBufferError, "DSERR_NOAGGREGATION");
      break;
    case DSERR_NODRIVER:
      rb_raise(eSoundBufferError, "DSERR_NODRIVER");
      break;
    case DSERR_NOINTERFACE:
      rb_raise(eSoundBufferError, "DSERR_NOINTERFACE");
      break;
    case DSERR_OBJECTNOTFOUND:
      rb_raise(eSoundBufferError, "DSERR_OBJECTNOTFOUND");
      break;
    case DSERR_OTHERAPPHASPRIO:
      rb_raise(eSoundBufferError, "DSERR_OTHERAPPHASPRIO");
      break;
    case DSERR_OUTOFMEMORY:
      rb_raise(eSoundBufferError, "DSERR_OUTOFMEMORY");
      break;
    case DSERR_PRIOLEVELNEEDED:
      rb_raise(eSoundBufferError, "DSERR_PRIOLEVELNEEDED");
      break;
    case DSERR_SENDLOOP:
      rb_raise(eSoundBufferError, "DSERR_SENDLOOP");
      break;
    case DSERR_UNINITIALIZED:
      rb_raise(eSoundBufferError, "DSERR_UNINITIALIZED");
      break;
    case DSERR_UNSUPPORTED:
      rb_raise(eSoundBufferError, "DSERR_UNSUPPORTED");
      break;
    case CO_E_NOTINITIALIZED:
      rb_raise(eSoundBufferError, "CO_E_NOTINITIALIZED");
      break;
    default:
      rb_raise(eSoundBufferError, "DirectSound API error"); // hrのHEX表示をつける
      break;
  }
}
