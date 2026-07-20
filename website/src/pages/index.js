import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import useBaseUrl from '@docusaurus/useBaseUrl';

const DOC_CARDS = [
  {
    to: '/docs/intro',
    icon: '📘',
    title: 'Overview & install',
    body: 'What this fork is, how to flash it, and how it differs from upstream sylverb.',
  },
  {
    to: '/docs/systems',
    icon: '🎮',
    title: 'Supported systems',
    body: 'NES, GB/C/A, SNES, Genesis, MSX, PCE, WonderSwan, and more — each with its own story.',
  },
  {
    to: '/docs/game-boy-advance',
    icon: '⚔️',
    title: 'Game Boy Advance',
    body: 'Pokémon running at full speed with M4A audio HLE on a 32-bit microcontroller.',
  },
  {
    to: '/docs/overclock-and-power',
    icon: '⚡',
    title: 'Overclock & power',
    body: 'How the device stays alive at 650 MHz and what it costs in battery life.',
  },
];

const FEATURED_DEVLOG = [
  {
    slug: 'super-metroid-three-releases-that-couldnt-boot',
    date: '2026-07-20',
    title: 'Super Metroid: three releases that couldn\u2019t boot',
    excerpt:
      'The host harness rendered 4,000 clean frames. The device hardfaulted on the first one. The same program on a different CPU is not the same program.',
    tags: ['snes', 'fault'],
  },
  {
    slug: 'six-overflows-in-a-toy-launcher',
    date: '2026-07-20',
    title: 'Six overflows in a toy launcher',
    excerpt:
      'It is a Game & Watch. It is not networked. There is nothing to attack. That is the lie I told myself until an adversarial pass found six buffer overflows \u2014 three of them hardfault-class on a long filename.',
    tags: ['fault', 'security'],
  },
  {
    slug: 'earthbound-the-memmove-that-ran-into-peripheral-space',
    date: '2026-07-20',
    title: 'EarthBound: the memmove that ran off into peripheral space',
    excerpt:
      'A 300 MB copy. A pointer in the size register. The compiler\u2019s register allocator, honouring an alignment annotation, silently shifted every argument one slot over.',
    tags: ['snes', 'fault'],
  },
  {
    slug: 'the-poll-was-innocent-the-caller-was-the-loop',
    date: '2026-07-20',
    title: 'GBA idle skip: the poll was innocent, the caller was the loop',
    excerpt:
      'A six-frame intro took seven hundred. The idle-skip table was correct \u2014 every entry in it was right. The detector had found the poll. It just could not tell spin from call.',
    tags: ['gba', 'performance'],
  },
  {
    slug: 'sega-cd-the-sub-cpu-that-waited-for-a-dead-main',
    date: '2026-07-20',
    title: 'Sega CD: the sub-CPU that waited for a dead main',
    excerpt:
      'The sub spun at $6132 waiting for a handshake. The main had died at $FF0000 \u2014 we had jumped it into the ASCII string "SEGA" and asked it to execute a header.',
    tags: ['segacd', 'fault'],
  },
  {
    slug: 'the-fader-the-game-forgot-to-cancel',
    date: '2026-07-20',
    title: 'PCE CD: the fader the game forgot to cancel',
    excerpt:
      'The music was silent. The rip was good. The CD-DA volume register read zero \u2014 because the game had faded out for a cinematic and never written the cancel before the BGM started.',
    tags: ['pce', 'audio'],
  },
  {
    slug: 'boot-rescue-when-a-hung-boot-was-a-dead-battery',
    date: '2026-07-20',
    title: 'Boot rescue: when a hung boot was a dead battery',
    excerpt:
      'No reset button. Power is a GPIO the firmware polls. A hang used to mean draining the battery. We made the watchdog count and stopped the third failed boot at a rescue screen.',
    tags: ['boot', 'fault'],
  },
  {
    slug: 'the-85kb-hash-table-that-didnt-fit-in-8kb-of-dtcm',
    date: '2026-07-20',
    title: 'The 85 KB hash table that did not fit in 8 KB of DTCM',
    excerpt:
      'The data structure was right. The heap was wrong. A runtime OOM that became a link error is the whole win \u2014 the budget moved from "the device tells you in a second" to "the linker tells you before you flash."',
    tags: ['snes', 'performance'],
  },
  {
    slug: 'clock-alarm-the-one-you-couldnt-turn-off',
    date: '2026-07-20',
    title: 'The clock alarm you couldn\u2019t turn off',
    excerpt:
      'Four separate device-only bugs, each reported as "the alarm went off and nothing would stop it." A confirm button wired as snooze. An RTC alarm that stayed armed while awake.',
    tags: ['clock', 'fault'],
  },
  {
    slug: 'the-roms-that-didnt-speed-up-are-the-data',
    date: '2026-07-20',
    title: '32X: the ROMs that did not speed up are the data',
    excerpt:
      'Seven ROMs sped up 28\u201344%. Eight did not move. The temptation is to declare victory on the seven. The discipline is to read the eight \u2014 they tell you the bottleneck is somewhere else.',
    tags: ['32x', 'performance'],
  },
  {
    slug: 'c64-when-cpp-exceptions-leaked-into-the-launcher',
    date: '2026-07-20',
    title: 'C64: when C++ exceptions leaked into the launcher',
    excerpt:
      'A C++ core\u2019s exception tables \u2014 for exceptions it never throws \u2014 quietly walked out of the overlay and into the resident launcher, eating 5.6 KB of a 44-byte budget.',
    tags: ['c64', 'hardware'],
  },
  {
    slug: '32x-fighting-for-1740-bytes-of-itcm',
    date: '2026-07-20',
    title: '32X: fighting for 1740 bytes of ITCM',
    excerpt:
      'The SH-2 interpreter would not fit in instruction TCM by 1740 bytes. The computed-goto dispatch could not be split. We had to move something cold out.',
    tags: ['32x', 'performance'],
  },
  {
    slug: 'virtual-boy-black-screen-crushed-audio-three-bugs-one-port',
    date: '2026-07-20',
    title: 'Virtual Boy: black screen, crushed audio, and the DRC that couldn\u2019t run',
    excerpt:
      'A silent divide-by-zero (ARM doesn\u2019t trap it, x86 does). A decimation without filtering. A DRC built for A32 on a Thumb-2-only core. Three bugs, one port, one week.',
    tags: ['fault', 'hardware'],
  },
  {
    slug: 'jpeg-decoder-the-last-byte-and-the-eoi-that-never-came',
    date: '2026-07-20',
    title: 'JPEG decoder: the last byte, and the EOI that never came',
    excerpt:
      'Nine frames in ten were rejected. The HAL was flooring the input length to a multiple of four \u2014 truncating the FF D9 end-of-image marker. Only the video player suffered.',
    tags: ['video', 'fault'],
  },
];

function Hero() {
  const {siteConfig} = useDocusaurusContext();
  const photo = useBaseUrl('/img/clock-hero.jpg');
  return (
    <header className={clsx('hero hero--lab')}>
      <div className="container">
        <h1 className="hero__title">🧪 G&amp;W Retro-Go Lab</h1>
        <p className="hero__subtitle">{siteConfig.tagline}</p>
        <div>
          <Link className="button button--secondary button--lg" to="/docs/intro">
            Read the docs →
          </Link>
          &nbsp;&nbsp;
          <Link className="button button--outline button--secondary button--lg" to="/devlog">
            Devlog
          </Link>
        </div>
        <div>
          <img className="hero__photo" src={photo} alt="The Clock app running on real Game & Watch hardware" />
          <p className="hero__caption">The built-in Clock app on real hardware.</p>
        </div>
      </div>
    </header>
  );
}

function DocCards() {
  return (
    <section className="gw-section gw-section--first">
      <div className="container">
        <p className="gw-lead">
          A <strong>personal experimental lab</strong> built on{' '}
          <a href="https://github.com/sylverb/game-and-watch-retro-go-sd">sylverb</a>&rsquo;s
          game-and-watch-retro-go-sd. Rough around the edges — a place to try things, not a
          &ldquo;better&rdquo; build. If you just want to play games, use the upstream stable
          release.
        </p>
        <p className="gw-eyebrow" style={{textAlign: 'center'}}>Start here</p>
        <h2 className="gw-section-title" style={{textAlign: 'center'}}>What&rsquo;s in this fork</h2>
        <div className="row">
          {DOC_CARDS.map((c) => (
            <div key={c.to} className="col col--3">
              <Link to={c.to} className="gw-card">
                <span className="gw-card__icon">{c.icon}</span>
                <div className="gw-card__title">{c.title}</div>
                <div className="gw-card__body">{c.body}</div>
              </Link>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function FeaturedDevlog() {
  return (
    <section className="gw-section">
      <div className="container">
        <p className="gw-eyebrow" style={{textAlign: 'center'}}>From the workbench</p>
        <h2 className="gw-section-title" style={{textAlign: 'center'}}>Latest devlog entries</h2>
        <div className="row">
          {FEATURED_DEVLOG.map((p) => (
            <div key={p.slug} className="col col--4">
              <Link className="gw-feature" to={`/devlog/${p.slug}`}>
                <div className="gw-feature__date">{p.date}</div>
                <div className="gw-feature__title">{p.title}</div>
                <p className="gw-feature__excerpt">{p.excerpt}</p>
                <div className="gw-feature__tags">
                  {p.tags.map((t) => (
                    <span key={t} className="gw-tag">#{t}</span>
                  ))}
                </div>
              </Link>
            </div>
          ))}
        </div>
        <p style={{textAlign: 'center', marginTop: '1.75rem'}}>
          <Link to="/devlog">All devlog entries →</Link>
        </p>
      </div>
    </section>
  );
}

export default function Home() {
  return (
    <Layout
      title="Home"
      description="An experimental fork of game-and-watch-retro-go-sd: extra systems (GBA, Super Metroid, …), apps and experiments on the Nintendo Game & Watch.">
      <Hero />
      <main className="margin-vert--lg">
        <DocCards />
        <FeaturedDevlog />
      </main>
    </Layout>
  );
}
