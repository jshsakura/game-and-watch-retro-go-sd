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
    slug: 'boot-rescue-when-a-hung-boot-was-a-dead-battery',
    date: '2026-07-20',
    title: 'Boot rescue: when a hung boot was a dead battery',
    excerpt:
      'No reset button. Power is a GPIO the firmware polls. A hang used to mean draining the battery. We made the watchdog count and stopped the third failed boot at a rescue screen.',
    tags: ['boot', 'fault'],
  },
  {
    slug: '32x-fighting-for-1740-bytes-of-itcm',
    date: '2026-07-20',
    title: '32X: fighting for 1740 bytes of ITCM',
    excerpt:
      'The SH-2 interpreter would not fit in instruction TCM by 1740 bytes. The computed-goto dispatch could not be split. We had to move something cold out.',
    tags: ['32x', 'performance'],
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
