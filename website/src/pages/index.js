import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import useBaseUrl from '@docusaurus/useBaseUrl';

function Hero() {
  const {siteConfig} = useDocusaurusContext();
  const photo = useBaseUrl('/img/clock-hero.jpg');
  return (
    <header className={clsx('hero hero--lab')}>
      <div className="container">
        <h1 className="hero__title">🧪 {siteConfig.title}</h1>
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

export default function Home() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title="Home"
      description="An experimental fork of game-and-watch-retro-go-sd: extra systems (GBA, Super Metroid, …), apps and experiments on the Nintendo Game & Watch.">
      <Hero />
      <main className="container margin-vert--lg">
        <div className="row">
          <div className="col col--8 col--offset-2">
            <p>
              This is a <strong>personal experimental lab</strong> built on top of{' '}
              <a href="https://github.com/sylverb/game-and-watch-retro-go-sd">sylverb/game-and-watch-retro-go-sd</a>.
              It is rough around the edges and a bit of a mess — a place to try things, not a
              &ldquo;better&rdquo; build. If you just want to play games, use sylverb&rsquo;s stable release.
            </p>
            <p>
              These pages document only what this fork <em>adds or changes</em>. Everything else —
              the hardware mod, installation, controls, per-emulator notes for the stock systems —
              lives in the upstream README and applies here unchanged.
            </p>
            <ul>
              <li><Link to="/docs/intro">Overview &amp; install</Link></li>
              <li><Link to="/docs/systems">Supported systems</Link></li>
              <li><Link to="/docs/game-boy-advance">Game Boy Advance (Pokémon full speed)</Link></li>
              <li><Link to="/docs/overclock-and-power">Overclock &amp; power</Link></li>
              <li><Link to="/devlog">Devlog</Link> — the development journal</li>
            </ul>
          </div>
        </div>
      </main>
    </Layout>
  );
}
